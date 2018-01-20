/* -------------------------------------------------------------------------- *
 * OpenSim Muscollo: sandboxCalibrateContact.cpp                              *
 * -------------------------------------------------------------------------- *
 * Copyright (c) 2017 Stanford University and the Authors                     *
 *                                                                            *
 * Author(s): Nicholas Bianco, Chris Dembia                                   *
 *                                                                            *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may    *
 * not use this file except in compliance with the License. You may obtain a  *
 * copy of the License at http://www.apache.org/licenses/LICENSE-2.0          *
 *                                                                            *
 * Unless required by applicable law or agreed to in writing, software        *
 * distributed under the License is distributed on an "AS IS" BASIS,          *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   *
 * See the License for the specific language governing permissions and        *
 * limitations under the License.                                             *
 * -------------------------------------------------------------------------- */


#include <OpenSim/Common/LogManager.h>
#include <OpenSim/Common/osimCommon.h>
#include <OpenSim/Simulation/osimSimulation.h>
#include <OpenSim/Actuators/osimActuators.h>
#include <OpenSim/Tools/InverseKinematicsTool.h>
#include <OpenSim/Common/TimeSeriesTable.h>
#include <Muscollo/osimMuscollo.h>

#include <MuscolloSandboxShared.h>

#include <tropter/tropter.h>

using namespace OpenSim;
using Eigen::VectorXd;

template <typename T, typename TStiffness>
T contact_force(const TStiffness& stiffness, const T& y) {
    const double stiffness_fictitious = 1.0; // N/m
    const T ground_height = 0;
    // Positive if penetrated.
    const T depth = ground_height - y;
    const T depth_pos = fmax(0, depth);
    const T contact_normal_force = stiffness * depth_pos +
            stiffness_fictitious * depth;
    return contact_normal_force;
}

template<typename T>
class BouncingBallLinear : public tropter::OptimalControlProblem<T> {
public:
    static const double mass; // kg
    static const double stiffness; // N/m
    static const double g; // m/s^2
    BouncingBallLinear() {
        this->set_time(0, 1.25);
        this->add_state("y", {-1, 1}, 1);
        this->add_state("vy", {-10, 10}, 0);
    }
    void calc_differential_algebraic_equations(
            const tropter::DAEInput<T>& in,
            tropter::DAEOutput<T> out) const override {
        const T& y = in.states[0];
        const T& vy = in.states[1];
        out.dynamics[0] = vy;
        const auto contact_normal_force = contact_force(this->stiffness, y);
        out.dynamics[1] = -g + (contact_normal_force) / mass;
    }
    static tropter::OptimalControlSolution run() {
        auto ocp = std::make_shared<BouncingBallLinear<T>>();
        const int N = 1000;
        tropter::DirectCollocationSolver<T> dircol(ocp, "trapezoidal", "ipopt",
                N);
        tropter::OptimalControlSolution solution = dircol.solve();
        //std::cout << "States: " << solution.states << std::endl;
        //solution.write("sandboxCalibrateContact_bouncing_ball_solution.csv");
        return solution;
    }
};
template <typename T>
const double BouncingBallLinear<T>::mass = 50.0; // kg
template <typename T>
const double BouncingBallLinear<T>::stiffness = 3180.0; // N/m
template <typename T>
const double BouncingBallLinear<T>::g = 9.81; // m/s^2

class BallCalibration : public tropter::OptimizationProblem<double> {
public:
    BallCalibration(Eigen::VectorXd yTraj, Eigen::VectorXd contactForceTraj) :
            tropter::OptimizationProblem<double>(1, 0),
            m_yTraj(yTraj), m_contactForceTraj(contactForceTraj) {
        Eigen::VectorXd lower(1); lower[0] = 0;
        Eigen::VectorXd upper(1); upper[0] = 10000;
        set_variable_bounds(lower, upper);
    }
    void calc_objective(const VectorXd& x, double& obj_value) const override {

        const auto& stiffness = x[0];
        obj_value = 0;
        for (int it = 0; it < m_yTraj.size(); ++it) {
            const auto& y = m_yTraj[it];
            const auto simContactForce = contact_force(stiffness, y);
            const auto& expContactForce = m_contactForceTraj[it];
            obj_value += pow(simContactForce - expContactForce, 2);
        }
    }
private:
    Eigen::VectorXd m_yTraj;
    Eigen::VectorXd m_contactForceTraj;
};

void calibrateBall() {
    const auto exp = BouncingBallLinear<adouble>::run();
    Eigen::VectorXd Fy(exp.time.size());
    for (int it = 0; it < exp.time.size(); ++it) {
        Fy[it] = contact_force(BouncingBallLinear<double>::stiffness,
                exp.states(0, it));
    }
    BallCalibration problem(exp.states.row(0).transpose(), Fy);
    tropter::IPOPTSolver solver(problem);
    solver.set_verbosity(1);
    auto solution = solver.optimize();
    std::cout << solution.variables << std::endl;
}

/// Optimize the stiffness of contact points to match the ground reaction
/// force.
class ContactCalibration : public tropter::OptimizationProblem<double> {
public:
    double forceScalingFactor = 1e8;
    ContactCalibration(Model model, StatesTrajectory statesTraj,
            int numContacts) :
            tropter::OptimizationProblem<double>(2 * numContacts, 0),
            m_model(std::move(model)), m_statesTraj(std::move(statesTraj)),
            m_numContacts(numContacts) {
        set_variable_bounds(Eigen::VectorXd::Zero(2 * numContacts),
                Eigen::VectorXd::Ones(2 * numContacts));

        m_model.initSystem();


        // Foot–ground contact data.
        // -------------------------
        auto data = STOFileAdapter::read("walk_gait1018_subject01_grf.mot");
        auto time = data.getIndependentColumn();
        SimTK::Vector Fx = data.getDependentColumn("ground_force_vx");
        SimTK::Vector Fy = data.getDependentColumn("ground_force_vy");
        // TODO GCVSpline FxSpline(5, (int)time.size(), time.data(), &Fx[0]);
        m_FySpline = GCVSpline(5, (int)time.size(), time.data(), &Fy[0]);
    }

    void applyParametersToModel(const VectorXd& x, Model& model) const {
        auto applyMarkerHeight = [&model](const std::string& name,
                                          const double& normHeight) {
            auto& marker = model.updComponent<Marker>(name);
            // index 1 for y component.
            const double lower = -0.06;
            const double upper =  0.05;
            marker.upd_location()[1] = lower + normHeight * (upper - lower);
        };
        for (int icontact = 0; icontact < m_numContacts; ++icontact) {
            const std::string name = "marker" + std::to_string(icontact);
            applyMarkerHeight(name, x[icontact]);
        }
        //std::cout << "DEBUG " <<
        //        model.getComponent<Marker>("R.Heel.Distal").get_location() <<
        //        std::endl;

        int icontact = 0;
        for (auto& contact :
                model.updComponentList<AckermannVanDenBogert2010Force>()) {
            contact.set_stiffness(
                    forceScalingFactor * x[icontact + m_numContacts]);
            ++icontact;
        }
    }

    void calc_objective(const VectorXd& x, double& obj_value) const override {

        obj_value = 0;

        //std::cout << "DEBUGx " << x << std::endl;
        //std::cout << "DEBUG " << std::setprecision(16) <<
        //        x[0] << "\n" << x[1] << "\n" << x[2] << "\n" <<
        //        x[3] << "\n" << x[4] << "\n" << x[5] << std::endl;

        const auto thread_id = std::this_thread::get_id();
        if (m_workingModels.count(thread_id) == 0) {
            std::unique_lock<std::mutex> lock(m_modelMutex);
            m_workingModels[thread_id] = m_model;
            lock.unlock();
            m_workingModels[thread_id].initSystem();
        }
        Model& model = m_workingModels[thread_id];

        // Apply parameters.
        // -----------------
        applyParametersToModel(x, model);
        // TODO is this expensive?
        model.initSystem();

        // Compute contact force error.
        // ----------------------------

        // TODO add fore-aft force.
        // TODO
//        std::cout << "DEBUG " <<
//                m_model.getComponent<Marker>("R.Heel.Distal").get_location() <<
//                std::endl;

        auto contacts =
                model.updComponentList<AckermannVanDenBogert2010Force>();
        // TODO
        //for (auto& contact : contacts) {
        //    std::cout << "DEBUGcalc_objective get_location: "
        //            << contact.getConnectee<Marker>("station").get_location()
        //            << std::endl;
        //}
        for (auto state : m_statesTraj) {
            // Making a copy of the state (auto state instead of const auto&
            // state) is important for invalidating the cached contact point
            // locations.
            model.realizeVelocity(state);
            SimTK::Real simFy = 0;

            for (auto& contact : contacts) {
                simFy += contact.calcContactForce(state)[1];
            }

            SimTK::Vector timeVec(1, state.getTime());
            SimTK::Real expFy = m_FySpline.calcValue(timeVec);
            //std::cout << "DEBUG " << simFy << " " << expFy << std::endl;
            obj_value += pow(simFy - expFy, 2);
        }
        const double mg = model.getTotalMass(m_statesTraj.front()) *
                        model.getGravity().norm();
        // TODO how should we normalize?
        obj_value /= mg * m_statesTraj.getSize();
    }
    void printContactComparison(const VectorXd& x,
            const std::string& filename) {
        TimeSeriesTable table;

        // Apply parameters.
        // -----------------
        applyParametersToModel(x, m_model);
        // TODO is this expensive?
        m_model.initSystem();

        // Compute contact force error.
        // ----------------------------

        // TODO add fore-aft force.

        auto contacts =
                m_model.updComponentList<AckermannVanDenBogert2010Force>();
        table.setColumnLabels({"simulation", "experiment"});
        for (auto state : m_statesTraj) {
            m_model.realizeVelocity(state);
            SimTK::RowVector row(2, 0.0);

            for (auto& contact : contacts) {
                row[0] += contact.calcContactForce(state)[1];
            }

            SimTK::Vector timeVec(1, state.getTime());
            row[1] = m_FySpline.calcValue(timeVec);

            table.appendRow(state.getTime(), row);
        }
        STOFileAdapter::write(table, filename);
    }
private:

    mutable Model m_model;
    StatesTrajectory m_statesTraj;
    GCVSpline m_FySpline;
    int m_numContacts;

    mutable std::mutex m_modelMutex;
    mutable std::unordered_map<std::thread::id, Model> m_workingModels;

};

class SimTKContactCalibration : public SimTK::OptimizerSystem {
public:
    SimTKContactCalibration(Model model, StatesTrajectory statesTraj,
            int numContacts)
            : SimTK::OptimizerSystem(2 * numContacts),
              m_tropProb(std::move(model), std::move(statesTraj), numContacts) {
        int N = 2 * numContacts;
        setParameterLimits(SimTK::Vector(N, 0.0), SimTK::Vector(N, 1.0));
    }
    void applyParametersToModel(const SimTK::Vector& vars, Model& model) const {
        Eigen::VectorXd x = Eigen::Map<const VectorXd>(&vars[0], vars.size());
        m_tropProb.applyParametersToModel(x, model);
    }
    int objectiveFunc(const SimTK::Vector& vars, bool, SimTK::Real& f)
    const override {
        ++m_objCount;
        Eigen::VectorXd x = Eigen::Map<const VectorXd>(&vars[0], vars.size());
        m_tropProb.calc_objective(x, f);
        std::cout << "DEBUG " << m_objCount << " " << f << " " << vars <<
                std::endl;
        return 0;
    }
    void printContactComparison(const SimTK::Vector& vars,
            const std::string& filename) {
        Eigen::VectorXd x = Eigen::Map<const VectorXd>(&vars[0], vars.size());
        m_tropProb.printContactComparison(x, filename);
    }
private:
    ContactCalibration m_tropProb;
    mutable std::atomic<int> m_objCount {0};
};

/// Convenience function to apply a CoordinateActuator to the model.
static void addCoordinateActuator(Model& model, std::string coordName,
        double optimalForce) {

    auto& coordSet = model.updCoordinateSet();

    CoordinateActuator* actu = nullptr;
    //auto* actActu = new ActivationCoordinateActuator();
    //actActu->set_default_activation(0.1);
    //actu = actActu;
    actu = new CoordinateActuator();
    actu->setName("tau_" + coordName);
    actu->setCoordinate(&coordSet.get(coordName));
    actu->setOptimalForce(optimalForce);
    model.addComponent(actu);
}

void addContact(Model& model, std::string markerName, double stiffness = 5e7) {
    //const double stiffness = 5e7;
    const double friction_coefficient = 0.95;
    const double velocity_scaling = 0.3;
    auto* contact = new AckermannVanDenBogert2010Force();
    contact->setName(markerName + "_contact");
    contact->set_stiffness(stiffness);
    contact->set_friction_coefficient(friction_coefficient);
    contact->set_tangent_velocity_scaling_factor(velocity_scaling);
    model.addComponent(contact);
    contact->updSocket("station").setConnecteeName(markerName);
}

void calibrateContact() {

    // Model.
    // ------
    Model model("gait1018_subject01_onefoot_v30516.osim");
    model.initSystem();

    addCoordinateActuator(model, "rz", 250);
    addCoordinateActuator(model, "tx", 5000);
    addCoordinateActuator(model, "ty", 5000);

    const auto& calcn = dynamic_cast<Body&>(model.updComponent("calcn_r"));
    /*
    model.addMarker(new Marker("R.Heel.Distal", calcn,
            SimTK::Vec3(0.01548, -0.0272884, -0.00503735)));
    model.addMarker(new Marker("R.Ball.Lat", calcn,
            SimTK::Vec3(0.16769, -0.0272884, 0.066)));
    model.addMarker(new Marker("R.Ball.Med", calcn,
            SimTK::Vec3(0.1898, -0.0272884, -0.03237)));
    addContact(model, "R.Heel.Distal", 5e7);
    addContact(model, "R.Ball.Lat", 7.5e7);
    addContact(model, "R.Ball.Med", 7.5e7);
    */
    // Programmatically add contact points across the foot.
    const SimTK::Real xHeel = -0.03;
    const SimTK::Real xToes =  0.28;
    const int numContacts = 6;
    for (int icontact = 0; icontact < numContacts; ++icontact) {
        const std::string name = "marker" + std::to_string(icontact);
        const SimTK::Real x = xHeel +
                SimTK::Real(icontact) / SimTK::Real(numContacts - 1) *
                        (xToes - xHeel);
        model.addMarker(new Marker(name, calcn, SimTK::Vec3(x, -0.027, 0.0)));
        addContact(model, name);

    }


    // Kinematics data.
    // ----------------
    StatesTrajectory statesTraj;
    std::unique_ptr<Storage> motion;
    {
        const std::string trcFile = "sandboxCalibrateContact_markers.trc";
        const std::string motFile = "sandboxCalibrateContact.mot";
        auto ref = TRCFileAdapter::read("walk_marker_trajectories.trc");
        // Convert from millimeters to meters.
        ref.updMatrix() /= 1000;
        const auto& reftime = ref.getIndependentColumn();
        const double walkingSpeed = 1.10; // m/s
        for (int i = 0; i < (int)ref.getNumColumns(); ++i) {
            SimTK::VectorView_<SimTK::Vec3> col =
                    ref.updDependentColumnAtIndex(i);
            for (int j = 0; j < col.size(); ++j) {
                col[j][0] += walkingSpeed * reftime[j]; // x
                col[j][1] -= 0.03; // y TODO
            }
        }
        TimeSeriesTable refFilt = filterLowpass(
                ref.flatten({ "_x", "_y", "_z" }), 6.0, true);
        {
            // Convert back to millimeters.
            TimeSeriesTableVec3 refMM(ref);
            refMM.updMatrix() *= 1000;
            TRCFileAdapter::write(refMM, trcFile);
        }

        InverseKinematicsTool ikTool;
        Model modelForIK(model);
        ikTool.setModel(modelForIK);
        ikTool.setMarkerDataFileName(trcFile);
        ikTool.setOutputMotionFileName(motFile);
        ikTool.run();

        motion.reset(new Storage(motFile));
        // TODO motion.pad(motion.getSize() / 2);
        motion->lowpassIIR(6.0);
        // Estimate speeds from coordinates;
        // see AnalyzeTool::loadStatesFromFile().
        Storage* qStore = nullptr;
        Storage* uStore = nullptr;
        SimTK::State s = model.initSystem();
        model.getSimbodyEngine().formCompleteStorages(s, *motion, qStore,
                uStore);
        model.getSimbodyEngine().convertDegreesToRadians(*qStore);
        model.getSimbodyEngine().convertDegreesToRadians(*uStore);
        uStore->addToRdStorage(*qStore,
                qStore->getFirstTime(), qStore->getLastTime());
        motion.reset(new Storage(512, "states"));
        model.formStateStorage(*qStore, *motion.get(), false);
        delete qStore;
        delete uStore;
        statesTraj = StatesTrajectory::createFromStatesStorage(model,
                *motion, true);
        // visualize(model, *motion);

        /*
        auto refPacked = refFilt.pack<double>();
        TimeSeriesTableVec3 refToUse(refPacked);

        Set<MarkerWeight> markerWeights;
        markerWeights.cloneAndAppend({ "R.Heel", 2 });
        markerWeights.cloneAndAppend({ "R.Toe.Tip", 2 });
        MarkersReference markersRef(refToUse, &markerWeights);

        MucoTool muco;
        MucoProblem& mp = muco.updProblem();
        mp.setModel(model);
        MucoBounds defaultSpeedBounds(-25, 25);
        mp.setTimeBounds(0.48, 1.8); // TODO [.58, 1.8] for gait cycle of right leg.
        mp.setStateInfo("ground_toes/rz/value", { -10, 10 });
        mp.setStateInfo("ground_toes/rz/speed", defaultSpeedBounds);
        mp.setStateInfo("ground_toes/tx/value", { -10, 10 });
        mp.setStateInfo("ground_toes/tx/speed", defaultSpeedBounds);
        mp.setStateInfo("ground_toes/ty/value", { -10, 10 });
        mp.setStateInfo("ground_toes/ty/speed", defaultSpeedBounds);
        mp.setControlInfo("tau_rz", { -1, 1 });
        mp.setControlInfo("tau_tx", { -1, 1 });
        mp.setControlInfo("tau_ty", { -1, 1 });

        MucoMarkerTrackingCost tracking;
        tracking.setMarkersReference(markersRef);
        tracking.setAllowUnusedReferences(true);
        tracking.setTrackedMarkerComponents("xy");
        mp.addCost(tracking);

        auto& ms = muco.initSolver();
        ms.set_num_mesh_points(30);
        ms.set_optim_convergence_tolerance(1e-2);
        ms.set_optim_constraint_tolerance(1e-2);

        mucoSol = muco.solve();
        statesTraj = mucoSol.exportToStatesTrajectory(mp);
        visualize(model, mucoSol.exportToStatesStorage());
         */

    }

    std::cout << "Number of states in trajectory: " << statesTraj.getSize()
            << std::endl;

    // IPOPT
    // -----
    /*
    ContactCalibration problem(model, statesTraj, numContacts);
    tropter::IPOPTSolver solver(problem);
    solver.set_verbosity(1);
    solver.set_max_iterations(100);
    //solver.set_hessian_approximation("exact");
    auto solution = solver.optimize();
    problem.applyParametersToModel(solution.variables, model);
    std::cout << solution.variables << std::endl;
    problem.printContactComparison(solution.variables,
            "sandboxCalibrateContact_comparison.sto");
    visualize(model, *motion);
     */

    // CMAES
    // -----
    SimTKContactCalibration sys(model, statesTraj, numContacts);
    SimTK::Vector results(2 * numContacts, 0.5);
    SimTK::Optimizer opt(sys, SimTK::CMAES);
    opt.setMaxIterations(3000);
    opt.setDiagnosticsLevel(3);
    opt.setConvergenceTolerance(1e-3);
    opt.setAdvancedRealOption("init_stepsize", 0.5);
    opt.setAdvancedStrOption("parallel", "multithreading");
    Stopwatch watch;
    double f = opt.optimize(results);
    std::cout << "objective: " << f << std::endl;
    std::cout << "variables: " << results << std::endl;
    std::cout << "Runtime: " << watch.getElapsedTimeFormatted() << std::endl;
    sys.printContactComparison(results,
            "sandboxCalibrateContact_comparison_cmaes.sto");
    sys.applyParametersToModel(results, model);
    visualize(model, *motion);
}


void toyCMAES() {
    class OptSys : public SimTK::OptimizerSystem {
    public:
        OptSys() : SimTK::OptimizerSystem(2) {}
        int objectiveFunc(const SimTK::Vector& vars, bool, SimTK::Real& f)
                const override {
            ++m_count;
            const auto id = std::this_thread::get_id();
            if (m_map.count(id) == 0) m_map[id] = 0;
            m_map[id]++;
            std::stringstream ss;
            ss << "DEBUG " << m_count << " " << id;
            std::cout << ss.str() << std::endl;
            const double x = vars[0];
            const double y = vars[1];
            f = 0.5 * (3 * x * x + 4 * x * y + 6 * y * y) - 2 * x + 8 * y;
            return 0;
        }
        mutable std::atomic<int> m_count;
        mutable std::unordered_map<std::thread::id, int> m_map;
    };
    OptSys sys;
    SimTK::Vector results(2);
    SimTK::Optimizer opt(sys, SimTK::CMAES);
    opt.setAdvancedStrOption("parallel", "multithreading");
    double f = opt.optimize(results);
    std::cout << "objective: " << f << std::endl;
    std::cout << "variables: " << results << std::endl;
    for (const auto& entry : sys.m_map) {
        std::cout << entry.second << std::endl;
    }
}

int main() {

    std::cout.rdbuf(LogManager::cout.rdbuf());
    std::cerr.rdbuf(LogManager::cerr.rdbuf());

    // calibrateBall();

    calibrateContact();

    // OpenSim::LogBuffer::sync() is not threadsafe.
    // toyCMAES();

    return EXIT_SUCCESS;
}

