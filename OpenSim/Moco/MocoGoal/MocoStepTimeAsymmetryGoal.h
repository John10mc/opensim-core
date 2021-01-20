#ifndef OPENSIM_MOCOSTEPTIMEASYMMETRYGOAL_H
#define OPENSIM_MOCOSTEPTIMEASYMMETRYGOAL_H
/* -------------------------------------------------------------------------- *
 * OpenSim: MocoStepTimeAsymmetryGoal.h                                       *
 * -------------------------------------------------------------------------- *
 * Copyright (c) 2020 Stanford University and the Authors                     *
 *                                                                            *
 * Author(s): Russell Johnson, Nicholas Bianco                                *
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

#include "MocoGoal.h"

namespace OpenSim {

class SmoothSphereHalfSpaceForce;

/* TODO does the user need to specify "front foot"
 * TODO try to reduce the number of properties
 * TODO what should be the range for target asymmetry?
@ingroup mocogoal */
class OSIMMOCO_API MocoStepTimeAsymmetryGoal : public MocoGoal {
    OpenSim_DECLARE_CONCRETE_OBJECT(MocoStepTimeAsymmetryGoal, MocoGoal);

public:
    MocoStepTimeAsymmetryGoal() { constructProperties(); }
    MocoStepTimeAsymmetryGoal(std::string name) : MocoGoal(std::move(name)) {
        constructProperties();
    }
    MocoStepTimeAsymmetryGoal(std::string name, double weight)
    : MocoGoal(std::move(name), weight) {
        constructProperties();
    }

protected:
    void initializeOnModelImpl(const Model&) const override;
    void calcIntegrandImpl(
            const IntegrandInput& input, double& integrand) const override;
    void calcGoalImpl(
            const GoalInput& input, SimTK::Vector& cost) const override;
//    void printDescriptionImpl() const override;

private:
    OpenSim_DECLARE_LIST_PROPERTY(left_contact_force_paths, std::string, "TODO");
    OpenSim_DECLARE_LIST_PROPERTY(right_contact_force_paths, std::string, "TODO");
    OpenSim_DECLARE_PROPERTY(left_foot_frame, std::string, "TODO");
    OpenSim_DECLARE_PROPERTY(right_foot_frame, std::string, "TODO");
    OpenSim_DECLARE_PROPERTY(contact_force_direction, std::string, "TODO. Default: 1.");
    OpenSim_DECLARE_PROPERTY(contact_force_threshold, double, "TODO");
    OpenSim_DECLARE_PROPERTY(walking_direction, std::string, "TODO");
    OpenSim_DECLARE_PROPERTY(smoothing, double, "TODO");
    OpenSim_DECLARE_PROPERTY(target_asymmetry, double, "TODO")

    void constructProperties();

    mutable std::vector<SimTK::ReferencePtr<const SmoothSphereHalfSpaceForce>>
        m_left_contacts;
    mutable std::vector<SimTK::ReferencePtr<const SmoothSphereHalfSpaceForce>>
        m_right_contacts;
    mutable SimTK::ReferencePtr<const Body> m_left_frame;
    mutable SimTK::ReferencePtr<const Body> m_right_frame;

    static const std::set<std::string> m_directions;
    mutable int m_walking_direction_index;
    mutable int m_walking_direction_sign;
    mutable int m_contact_force_index;
    mutable int m_contact_force_sign;

    using ConditionalFunction =
        double(const double&, const double&, const double&, const double&);
    mutable std::function<ConditionalFunction> m_conditional;
};

} // namespace OpenSim

#endif // OPENSIM_MOCOSTEPTIMEASYMMETRYGOAL_H
