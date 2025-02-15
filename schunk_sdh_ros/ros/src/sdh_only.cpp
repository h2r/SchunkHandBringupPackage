/*
 * Copyright 2017 Fraunhofer Institute for Manufacturing Engineering and Automation (IPA)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


// ##################
// #### includes ####
// standard includes
#include <unistd.h>
#include <map>
#include <string>
#include <vector>

// ROS includes
#include <ros/ros.h>
#include <urdf/model.h>
#include <actionlib/server/simple_action_server.h>

// ROS message includes
#include <std_msgs/Float64MultiArray.h>
#include <trajectory_msgs/JointTrajectory.h>
#include <sensor_msgs/JointState.h>
#include <control_msgs/FollowJointTrajectoryAction.h>
#include <control_msgs/JointTrajectoryControllerState.h>
#include <schunk_sdh/TemperatureArray.h>

// ROS service includes
#include <std_srvs/Trigger.h>
#include <cob_srvs/SetString.h>

// ROS diagnostic msgs
#include <diagnostic_msgs/DiagnosticArray.h>

// external includes
#include <schunk_sdh/sdh.h>
#include <schunk_sdh/util.h>
/*!
 * \brief Implementation of ROS node for sdh.
 *
 * Offers actionlib and direct command interface.
 */
class SdhNode
{
public:
  /// create a handle for this node, initialize node
  ros::NodeHandle nh_;

private:
  // declaration of topics to publish
  ros::Publisher topicPub_JointState_;
  ros::Publisher topicPub_ControllerState_;
  ros::Publisher topicPub_Diagnostics_;
  ros::Publisher topicPub_Temperature_;

  // topic subscribers
  ros::Subscriber subSetVelocitiesRaw_;

  // service servers
  ros::ServiceServer srvServer_Init_;
  ros::ServiceServer srvServer_Stop_;
  ros::ServiceServer srvServer_Recover_;
  ros::ServiceServer srvServer_SetOperationMode_;
  ros::ServiceServer srvServer_EmergencyStop_;
  ros::ServiceServer srvServer_Disconnect_;
  ros::ServiceServer srvServer_MotorOn_;
  ros::ServiceServer srvServer_MotorOff_;

  // actionlib server
  actionlib::SimpleActionServer<control_msgs::FollowJointTrajectoryAction> as_;
  std::string action_name_;

  // service clients
  // --

  // other variables
  SDH::cSDH *sdh_;
  std::vector<SDH::cSDH::eAxisState> state_;

  std::string sdhdevicetype_;
  std::string sdhdevicestring_;
  int sdhdevicenum_;
  int baudrate_, id_read_, id_write_, sdh_port_;
  double timeout_;

  bool isInitialized_;
  bool isError_;
  int DOF_;
  double pi_;

  trajectory_msgs::JointTrajectory traj_;

  std::vector<std::string> joint_names_;
  std::vector<int> axes_;
  std::vector<double> targetAngles_;  // in degrees
  std::vector<double> velocities_;  // in rad/s
  bool hasNewGoal_;
  std::string operationMode_;
  std::vector<double> max_velocities_;

  static const std::vector<std::string> temperature_names_;

public:
  /*!
   * \brief Constructor for SdhNode class
   *
   * \param name Name for the actionlib server
   */
  SdhNode(std::string name) :
      as_(nh_, name, boost::bind(&SdhNode::executeCB, this, _1), false), action_name_(name)
  {
    nh_ = ros::NodeHandle("~");
    pi_ = 3.1415926;
    isError_ = false;

    as_.start();
  }

  /*!
   * \brief Destructor for SdhNode class
   */
  ~SdhNode()
  {
    if (isInitialized_)
      sdh_->Close();
    delete sdh_;
  }

  /*!
   * \brief Initializes node to get parameters, subscribe and publish to topics.
   */
  bool init()
  {
    // initialize member variables
    isInitialized_ = false;
    hasNewGoal_ = false;

    // implementation of topics to publish
    topicPub_JointState_ = nh_.advertise<sensor_msgs::JointState>("joint_states", 1);
    topicPub_ControllerState_ = nh_.advertise<control_msgs::JointTrajectoryControllerState>(
        "joint_trajectory_controller/state", 1);
    topicPub_Diagnostics_ = nh_.advertise<diagnostic_msgs::DiagnosticArray>("/diagnostics", 1);
    topicPub_Temperature_ = nh_.advertise<schunk_sdh::TemperatureArray>("temperature", 1);

    // pointer to sdh
    sdh_ = new SDH::cSDH(false, false, 0);  // (_use_radians=false, bool _use_fahrenheit=false, int _debug_level=0)

    // implementation of service servers
    srvServer_Init_ = nh_.advertiseService("init", &SdhNode::srvCallback_Init, this);
    srvServer_Stop_ = nh_.advertiseService("stop", &SdhNode::srvCallback_Stop, this);
    srvServer_Recover_ = nh_.advertiseService("recover", &SdhNode::srvCallback_Init, this);  // HACK: There is no recover implemented yet, so we execute a init
    srvServer_SetOperationMode_ = nh_.advertiseService("set_operation_mode",
                                                       &SdhNode::srvCallback_SetOperationMode, this);

    srvServer_EmergencyStop_ = nh_.advertiseService("emergency_stop", &SdhNode::srvCallback_EmergencyStop, this);
    srvServer_Disconnect_ = nh_.advertiseService("shutdown", &SdhNode::srvCallback_Disconnect, this);

    srvServer_MotorOn_ = nh_.advertiseService("motor_on", &SdhNode::srvCallback_MotorPowerOn, this);
    srvServer_MotorOff_ = nh_.advertiseService("motor_off", &SdhNode::srvCallback_MotorPowerOff, this);

    subSetVelocitiesRaw_ = nh_.subscribe("joint_group_velocity_controller/command", 1,
                                         &SdhNode::topicCallback_setVelocitiesRaw, this);

    // getting hardware parameters from parameter server
    nh_.param("sdhdevicetype", sdhdevicetype_, std::string("TCP"));
    nh_.param("sdhdevicestring", sdhdevicestring_, std::string("192.168.1.42"));
    nh_.param("sdhdevicenum", sdhdevicenum_, 0);
    nh_.param("sdhport", sdh_port_, 23);
    nh_.param("baudrate", baudrate_, 1000000);
    nh_.param("timeout", timeout_, static_cast<double>(0.04));
    nh_.param("id_read", id_read_, 43);
    nh_.param("id_write", id_write_, 42);

    // get joint_names from parameter server
    ROS_INFO("getting joint_names from parameter server");
    XmlRpc::XmlRpcValue joint_names_param;
    if (nh_.hasParam("joint_names"))
    {
      nh_.getParam("joint_names", joint_names_param);
    }
    else
    {
      ROS_ERROR("Parameter 'joint_names' not set, shutting down node...");
      nh_.shutdown();
      return false;
    }
    DOF_ = joint_names_param.size();
    joint_names_.resize(DOF_);
    for (int i = 0; i < DOF_; i++)
    {
      joint_names_[i] = (std::string)joint_names_param[i];
    }
    std::cout << "joint_names = " << joint_names_param << std::endl;

    // define axes to send to sdh
    axes_.resize(DOF_);
    velocities_.resize(DOF_);
    for (int i = 0; i < DOF_; i++)
    {
      axes_[i] = i;
    }
    ROS_INFO("DOF = %d", DOF_);

    state_.resize(axes_.size());

    nh_.param("OperationMode", operationMode_, std::string("position"));
    return true;
  }
  /*!
   * \brief Switches operation mode if possible
   *
   * \param mode new mode
   */
  bool switchOperationMode(const std::string &mode)
  {
    hasNewGoal_ = false;
    sdh_->Stop();

    try
    {
      if (mode == "position")
      {
        sdh_->SetController(SDH::cSDH::eCT_POSE);
      }
      else if (mode == "velocity")
      {
        sdh_->SetController(SDH::cSDH::eCT_VELOCITY);
      }
      else
      {
        ROS_ERROR_STREAM("Operation mode '" << mode << "'  not supported");
        return false;
      }
      sdh_->SetAxisEnable(sdh_->All, 1.0);  // TODO: check if necessary
    }
    catch (SDH::cSDHLibraryException* e)
    {
      ROS_ERROR("An exception was caught: %s", e->what());
      delete e;
      return false;
    }

    operationMode_ = mode;
    return true;
  }

  /*!
   * \brief Executes the callback from the actionlib
   *
   * Set the current goal to aborted after receiving a new goal and write new goal to a member variable. Wait for the goal to finish and set actionlib status to succeeded.
   * \param goal JointTrajectoryGoal
   */
  void executeCB(const control_msgs::FollowJointTrajectoryGoalConstPtr &goal)
  {
    ROS_INFO("sdh: executeCB");
    if (operationMode_ != "position")
    {
      ROS_ERROR("%s: Rejected, sdh not in position mode", action_name_.c_str());
      as_.setAborted();
      return;
    }
    if (!isInitialized_)
    {
      ROS_ERROR("%s: Rejected, sdh not initialized", action_name_.c_str());
      as_.setAborted();
      return;
    }

    if (goal->trajectory.points.empty() || goal->trajectory.points[0].positions.size() != size_t(DOF_))
    {
      ROS_ERROR("%s: Rejected, malformed FollowJointTrajectoryGoal", action_name_.c_str());
      as_.setAborted();
      return;
    }
    while (hasNewGoal_ == true)
      usleep(10000);

    std::map<std::string, int> dict;
    for (int idx = 0; idx < goal->trajectory.joint_names.size(); idx++)
    {
      dict[goal->trajectory.joint_names[idx]] = idx;
    }

    targetAngles_.resize(DOF_);
    targetAngles_[0] = goal->trajectory.points[0].positions[dict["sdh_knuckle_joint"]] * 180.0 / pi_;  // sdh_knuckle_joint
    targetAngles_[1] = goal->trajectory.points[0].positions[dict["sdh_finger_22_joint"]] * 180.0 / pi_;  // sdh_finger22_joint
    targetAngles_[2] = goal->trajectory.points[0].positions[dict["sdh_finger_23_joint"]] * 180.0 / pi_;  // sdh_finger23_joint
    targetAngles_[3] = goal->trajectory.points[0].positions[dict["sdh_thumb_2_joint"]] * 180.0 / pi_;  // sdh_thumb2_joint
    targetAngles_[4] = goal->trajectory.points[0].positions[dict["sdh_thumb_3_joint"]] * 180.0 / pi_;  // sdh_thumb3_joint
    targetAngles_[5] = goal->trajectory.points[0].positions[dict["sdh_finger_12_joint"]] * 180.0 / pi_;  // sdh_finger12_joint
    targetAngles_[6] = goal->trajectory.points[0].positions[dict["sdh_finger_13_joint"]] * 180.0 / pi_;  // sdh_finger13_joint
    ROS_INFO(
        "received position goal: [['sdh_knuckle_joint', 'sdh_thumb_2_joint', 'sdh_thumb_3_joint', 'sdh_finger_12_joint', 'sdh_finger_13_joint', 'sdh_finger_22_joint', 'sdh_finger_23_joint']] = [%f,%f,%f,%f,%f,%f,%f]",
        goal->trajectory.points[0].positions[dict["sdh_knuckle_joint"]],
        goal->trajectory.points[0].positions[dict["sdh_thumb_2_joint"]],
        goal->trajectory.points[0].positions[dict["sdh_thumb_3_joint"]],
        goal->trajectory.points[0].positions[dict["sdh_finger_12_joint"]],
        goal->trajectory.points[0].positions[dict["sdh_finger_13_joint"]],
        goal->trajectory.points[0].positions[dict["sdh_finger_22_joint"]],
        goal->trajectory.points[0].positions[dict["sdh_finger_23_joint"]]);

    hasNewGoal_ = true;

    usleep(500000);  // needed sleep until sdh starts to change status from idle to moving

    bool finished = false;
    while (finished == false)
    {
      if (as_.isNewGoalAvailable())
      {
        ROS_WARN("%s: Aborted", action_name_.c_str());
        as_.setAborted();
        return;
      }
      for (unsigned int i = 0; i < state_.size(); i++)
      {
        ROS_DEBUG("state[%d] = %d", i, state_[i]);
        if (state_[i] == 0)
        {
          finished = true;
        }
        else
        {
          finished = false;
        }
      }
      usleep(10000);
    }

    // set the action state to succeeded
    ROS_INFO("%s: Succeeded", action_name_.c_str());
    as_.setSucceeded();
  }

  void topicCallback_setVelocitiesRaw(const std_msgs::Float64MultiArrayPtr& velocities)
  {
    if (!isInitialized_)
    {
      ROS_ERROR("%s: Rejected, sdh not initialized", action_name_.c_str());
      return;
    }
    if (velocities->data.size() != velocities_.size())
    {
      ROS_ERROR("Velocity array dimension mismatch");
      return;
    }
    if (operationMode_ != "velocity")
    {
      ROS_ERROR("%s: Rejected, sdh not in velocity mode", action_name_.c_str());
      return;
    }

    // TODO: write proper lock!
    while (hasNewGoal_ == true)
      usleep(10000);

    velocities_[0] = velocities->data[0] * 180.0 / pi_;  // sdh_knuckle_joint
    velocities_[1] = velocities->data[5] * 180.0 / pi_;  // sdh_finger22_joint
    velocities_[2] = velocities->data[6] * 180.0 / pi_;  // sdh_finger23_joint
    velocities_[3] = velocities->data[1] * 180.0 / pi_;  // sdh_thumb2_joint
    velocities_[4] = velocities->data[2] * 180.0 / pi_;  // sdh_thumb3_joint
    velocities_[5] = velocities->data[3] * 180.0 / pi_;  // sdh_finger12_joint
    velocities_[6] = velocities->data[4] * 180.0 / pi_;  // sdh_finger13_joint

    hasNewGoal_ = true;
  }

  /*!
   * \brief Executes the service callback for init.
   *
   * Connects to the hardware and initialized it.
   * \param req Service request
   * \param res Service response
   */
  bool srvCallback_Init(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
  {
    if (isInitialized_ == false)
    {
      // Init Hand connection

      try
      {
        if (sdhdevicetype_.compare("RS232") == 0)
        {
          sdh_->OpenRS232(sdhdevicenum_, 115200, 1, sdhdevicestring_.c_str());
          ROS_INFO("Initialized RS232 for SDH");
          isInitialized_ = true;
        }
        if (sdhdevicetype_.compare("PCAN") == 0)
        {
          ROS_INFO("Starting initializing PEAKCAN");
          sdh_->OpenCAN_PEAK(baudrate_, timeout_, id_read_, id_write_, sdhdevicestring_.c_str());
          ROS_INFO("Initialized PEAK CAN for SDH");
          isInitialized_ = true;
        }
        if(sdhdevicetype_.compare("TCP") == 0)
        {
			ROS_INFO("Starting initializing TCP");
            sdh_->OpenTCP(sdhdevicestring_.c_str(), sdh_port_, timeout_);
            ROS_INFO("Initialized TCP for SDH");
            isInitialized_ = true;
			
		}
        if (sdhdevicetype_.compare("ESD") == 0)
        {
          ROS_INFO("Starting initializing ESD");
          if (strcmp(sdhdevicestring_.c_str(), "/dev/can0") == 0)
          {
            ROS_INFO("Initializing ESD on device %s", sdhdevicestring_.c_str());
            sdh_->OpenCAN_ESD(0, baudrate_, timeout_, id_read_, id_write_);
          }
          else if (strcmp(sdhdevicestring_.c_str(), "/dev/can1") == 0)
          {
            ROS_INFO("Initializin ESD on device %s", sdhdevicestring_.c_str());
            sdh_->OpenCAN_ESD(1, baudrate_, timeout_, id_read_, id_write_);
          }
          else
          {
            ROS_ERROR("Currently only support for /dev/can0 and /dev/can1");
            res.success = false;
            res.message = "Currently only support for /dev/can0 and /dev/can1";
            return true;
          }
          ROS_INFO("Initialized ESDCAN for SDH");
          isInitialized_ = true;
        }

        max_velocities_ = sdh_->GetAxisMaxVelocity( sdh_->all_real_axes );
      }
      catch (SDH::cSDHLibraryException* e)
      {
        ROS_ERROR("An exception was caught: %s", e->what());
        res.success = false;
        res.message = e->what();
        delete e;
        return true;
      }
      if (!switchOperationMode(operationMode_))
      {
        res.success = false;
        res.message = "Could not set operation mode to '" + operationMode_ + "'";
        return true;
      }
    }
    else
    {
      ROS_WARN("...sdh already initialized...");
      res.success = true;
      res.message = "sdh already initialized";
    }

    res.success = true;
    return true;
  }

  /*!
   * \brief Executes the service callback for stop.
   *
   * Stops all hardware movements.
   * \param req Service request
   * \param res Service response
   */
  bool srvCallback_Stop(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
  {
    ROS_INFO("Stopping sdh");

    // stopping all arm movements
    try
    {
      sdh_->Stop();
    }
    catch (SDH::cSDHLibraryException* e)
    {
      ROS_ERROR("An exception was caught: %s", e->what());
      delete e;
    }

    ROS_INFO("Stopping sdh succesfull");
    res.success = true;
    return true;
  }

  /*!
   * \brief Executes the service callback for recover.
   *
   * Recovers the hardware after an emergency stop.
   * \param req Service request
   * \param res Service response
   */
  bool srvCallback_Recover(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res)
  {
    ROS_WARN("Service recover not implemented yet");
    res.success = true;
    res.message = "Service recover not implemented yet";
    return true;
  }

  /*!
   * \brief Executes the service callback for set_operation_mode.
   *
   * Changes the operation mode.
   * \param req Service request
   * \param res Service response
   */
  bool srvCallback_SetOperationMode(cob_srvs::SetString::Request &req, cob_srvs::SetString::Response &res)
  {
    hasNewGoal_ = false;
    sdh_->Stop();
    res.success = switchOperationMode(req.data);
    if (operationMode_ == "position")
    {
      sdh_->SetController(SDH::cSDH::eCT_POSE);
    }
    else if (operationMode_ == "velocity")
    {
      try
      {
        sdh_->SetController(SDH::cSDH::eCT_VELOCITY);
        sdh_->SetAxisEnable(sdh_->All, 1.0);
      }
      catch (SDH::cSDHLibraryException* e)
      {
        ROS_ERROR("An exception was caught: %s", e->what());
        delete e;
      }
    }
    else
    {
      ROS_ERROR_STREAM("Operation mode '" << req.data << "'  not supported");
    }
    return true;
  }

  /*!
   * \brief Executes the service callback for emergency_stop.
   *
   * Performs an emergency stop.
   * \param req Service request
   * \param res Service response
   */
  bool srvCallback_EmergencyStop(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res) {
      try {
        isInitialized_ = false;
        sdh_->EmergencyStop();
        sdh_->SetAxisEnable(sdh_->All, 0.0);
        sdh_->SetAxisMotorCurrent(sdh_->All, 0.0);
      }
      catch(const SDH::cSDHLibraryException* e) {
          ROS_ERROR("An exception was caught: %s", e->what());
          res.success = false;
          res.message = e->what();
          return true;
      }

      res.success = true;
      res.message = "EMERGENCY stop";
      return true;
  }

  /*!
   * \brief Executes the service callback for disconnect.
   *
   * Disconnect from SDH and disable motors to prevent overheating.
   * \param req Service request
   * \param res Service response
   */
  bool srvCallback_Disconnect(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res) {
      try {
        isInitialized_ = false;

        sdh_->SetAxisEnable(sdh_->All, 0.0);
        sdh_->SetAxisMotorCurrent(sdh_->All, 0.0);

        sdh_->Close();
      }
      catch(const SDH::cSDHLibraryException* e) {
          ROS_ERROR("An exception was caught: %s", e->what());
          res.success = false;
          res.message = e->what();
          return true;
      }

      ROS_INFO("Disconnected");
      res.success = true;
      res.message = "disconnected from SDH";
      return true;
  }

  /*!
   * \brief Enable motor power
   * \param req Service request
   * \param res Service response
   */
  bool srvCallback_MotorPowerOn(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res) {
    try {
      sdh_->SetAxisEnable(sdh_->All, 1.0);
      sdh_->SetAxisMotorCurrent(sdh_->All, 0.5);
    }
    catch (const SDH::cSDHLibraryException* e) {
      ROS_ERROR("An exception was caught: %s", e->what());
      res.success = false;
      res.message = e->what();
      return true;
    }
    ROS_INFO("Motor power ON");
    res.success = true;
    res.message = "Motor ON";
    return true;
  }

  /*!
   * \brief Disable motor power
   * \param req Service request
   * \param res Service response
   */
  bool srvCallback_MotorPowerOff(std_srvs::Trigger::Request &req, std_srvs::Trigger::Response &res) {
    try {
      sdh_->SetAxisEnable(sdh_->All, 0.0);
      sdh_->SetAxisMotorCurrent(sdh_->All, 0.0);
    }
    catch (const SDH::cSDHLibraryException* e) {
      ROS_ERROR("An exception was caught: %s", e->what());
      res.success = false;
      res.message = e->what();
      return true;
    }
    ROS_INFO("Motor power OFF");
    res.success = true;
    res.message = "Motor OFF";
    return true;
  }


  void clampVelocities()
  {
	  for (int i=0; i< max_velocities_.size(); i++){

		  velocities_[i] = SDH::ToRange(velocities_[i], -max_velocities_[i], max_velocities_[i]);
	  }

  }

  /*!
   * \brief Main routine to update sdh.
   *
   * Sends target to hardware and reads out current configuration.
   */
  void updateSdh()
  {
    ROS_DEBUG("updateJointState");
    if (isInitialized_ == true)
    {
      if (hasNewGoal_ == true)
      {
        // stop sdh first when new goal arrived
        try
        {
          sdh_->Stop();
        }
        catch (SDH::cSDHLibraryException* e)
        {
          ROS_ERROR("An exception was caught: %s", e->what());
          delete e;
        }

        if (operationMode_ == "position")
        {
          ROS_DEBUG("moving sdh in position mode");

          try
          {
            sdh_->SetAxisTargetAngle(axes_, targetAngles_);
            sdh_->MoveHand(false);
          }
          catch (SDH::cSDHLibraryException* e)
          {
            ROS_ERROR("An exception was caught: %s", e->what());
            delete e;
          }
        }
        else if (operationMode_ == "velocity")
        {
          ROS_DEBUG("moving sdh in velocity mode");
          try
          {
        	clampVelocities();
            sdh_->SetAxisTargetVelocity(axes_, velocities_);
            // ROS_DEBUG_STREAM("velocities: " << velocities_[0] << " "<< velocities_[1] << " "<< velocities_[2] << " "<< velocities_[3] << " "<< velocities_[4] << " "<< velocities_[5] << " "<< velocities_[6]);
          }
          catch (SDH::cSDHLibraryException* e)
          {
            ROS_ERROR("An exception was caught: %s", e->what());
            delete e;
          }
        }
        else if (operationMode_ == "effort")
        {
          ROS_DEBUG("moving sdh in effort mode");
          // sdh_->MoveVel(goal->trajectory.points[0].velocities);
          ROS_WARN("Moving in effort mode currently disabled");
        }
        else
        {
          ROS_ERROR("sdh neither in position nor in velocity nor in effort mode. OperationMode = [%s]",
                    operationMode_.c_str());
        }

        hasNewGoal_ = false;
      }

      // read and publish joint angles and velocities
      std::vector<double> actualAngles;
      try
      {
        actualAngles = sdh_->GetAxisActualAngle(axes_);
      }
      catch (SDH::cSDHLibraryException* e)
      {
        ROS_ERROR("An exception was caught: %s", e->what());
        delete e;
      }
      std::vector<double> actualVelocities;
      try
      {
        actualVelocities = sdh_->GetAxisActualVelocity(axes_);
      }
      catch (SDH::cSDHLibraryException* e)
      {
        ROS_ERROR("An exception was caught: %s", e->what());
        delete e;
      }

      ROS_DEBUG("received %d angles from sdh", static_cast<int>(actualAngles.size()));

      ros::Time time = ros::Time::now();

      // create joint_state message
      sensor_msgs::JointState msg;
      msg.header.stamp = time;
      msg.name.resize(DOF_);
      msg.position.resize(DOF_);
      msg.velocity.resize(DOF_);
      msg.effort.resize(DOF_);
      // set joint names and map them to angles
      msg.name = joint_names_;
      // ['sdh_knuckle_joint', 'sdh_thumb_2_joint', 'sdh_thumb_3_joint', 'sdh_finger_12_joint', 'sdh_finger_13_joint', 'sdh_finger_22_joint', 'sdh_finger_23_joint']
      // pos
      msg.position[0] = actualAngles[0] * pi_ / 180.0;  // sdh_knuckle_joint
      msg.position[1] = actualAngles[3] * pi_ / 180.0;  // sdh_thumb_2_joint
      msg.position[2] = actualAngles[4] * pi_ / 180.0;  // sdh_thumb_3_joint
      msg.position[3] = actualAngles[5] * pi_ / 180.0;  // sdh_finger_12_joint
      msg.position[4] = actualAngles[6] * pi_ / 180.0;  // sdh_finger_13_joint
      msg.position[5] = actualAngles[1] * pi_ / 180.0;  // sdh_finger_22_joint
      msg.position[6] = actualAngles[2] * pi_ / 180.0;  // sdh_finger_23_joint
      // vel
      msg.velocity[0] = actualVelocities[0] * pi_ / 180.0;  // sdh_knuckle_joint
      msg.velocity[1] = actualVelocities[3] * pi_ / 180.0;  // sdh_thumb_2_joint
      msg.velocity[2] = actualVelocities[4] * pi_ / 180.0;  // sdh_thumb_3_joint
      msg.velocity[3] = actualVelocities[5] * pi_ / 180.0;  // sdh_finger_12_joint
      msg.velocity[4] = actualVelocities[6] * pi_ / 180.0;  // sdh_finger_13_joint
      msg.velocity[5] = actualVelocities[1] * pi_ / 180.0;  // sdh_finger_22_joint
      msg.velocity[6] = actualVelocities[2] * pi_ / 180.0;  // sdh_finger_23_joint
      // publish message
      topicPub_JointState_.publish(msg);

      // because the robot_state_publisher doesn't know about the mimic joint, we have to publish the coupled joint separately
      sensor_msgs::JointState mimicjointmsg;
      mimicjointmsg.header.stamp = time;
      mimicjointmsg.name.resize(1);
      mimicjointmsg.position.resize(1);
      mimicjointmsg.velocity.resize(1);
      mimicjointmsg.name[0] = "schunk_right_finger_21_joint";
      mimicjointmsg.position[0] = msg.position[0];  // sdh_knuckle_joint = sdh_finger_21_joint
      mimicjointmsg.velocity[0] = msg.velocity[0];  // sdh_knuckle_joint = sdh_finger_21_joint
      topicPub_JointState_.publish(mimicjointmsg);

      // publish controller state message
      control_msgs::JointTrajectoryControllerState controllermsg;
      controllermsg.header.stamp = time;
      controllermsg.joint_names.resize(DOF_);
      controllermsg.desired.positions.resize(DOF_);
      controllermsg.desired.velocities.resize(DOF_);
      controllermsg.actual.positions.resize(DOF_);
      controllermsg.actual.velocities.resize(DOF_);
      controllermsg.error.positions.resize(DOF_);
      controllermsg.error.velocities.resize(DOF_);
      // set joint names and map them to angles
      controllermsg.joint_names = joint_names_;
      // ['sdh_knuckle_joint', 'sdh_thumb_2_joint', 'sdh_thumb_3_joint', 'sdh_finger_12_joint', 'sdh_finger_13_joint', 'sdh_finger_22_joint', 'sdh_finger_23_joint']
      // desired pos
      if (targetAngles_.size() != 0)
      {
        controllermsg.desired.positions[0] = targetAngles_[0] * pi_ / 180.0;  // sdh_knuckle_joint
        controllermsg.desired.positions[1] = targetAngles_[3] * pi_ / 180.0;  // sdh_thumb_2_joint
        controllermsg.desired.positions[2] = targetAngles_[4] * pi_ / 180.0;  // sdh_thumb_3_joint
        controllermsg.desired.positions[3] = targetAngles_[5] * pi_ / 180.0;  // sdh_finger_12_joint
        controllermsg.desired.positions[4] = targetAngles_[6] * pi_ / 180.0;  // sdh_finger_13_joint
        controllermsg.desired.positions[5] = targetAngles_[1] * pi_ / 180.0;  // sdh_finger_22_joint
        controllermsg.desired.positions[6] = targetAngles_[2] * pi_ / 180.0;  // sdh_finger_23_joint
      }
      // desired vel
      // they are all zero
      // actual pos
      controllermsg.actual.positions = msg.position;
      // actual vel
      controllermsg.actual.velocities = msg.velocity;
      // error, calculated out of desired and actual values
      for (int i = 0; i < DOF_; i++)
      {
        controllermsg.error.positions[i] = controllermsg.desired.positions[i] - controllermsg.actual.positions[i];
        controllermsg.error.velocities[i] = controllermsg.desired.velocities[i] - controllermsg.actual.velocities[i];
      }
      // publish controller message
      topicPub_ControllerState_.publish(controllermsg);

      // read sdh status
      state_ = sdh_->GetAxisActualState(axes_);

      // publish temperature
      schunk_sdh::TemperatureArray temp_array;
      temp_array.header.stamp = time;
      const std::vector<double> temp_value = sdh_->GetTemperature(sdh_->all_temperature_sensors);
      if(temp_value.size()==temperature_names_.size()) {
          temp_array.name = temperature_names_;
          temp_array.temperature = temp_value;
      }
      else {
          ROS_ERROR("amount of temperatures mismatch with stored names");
      }
      topicPub_Temperature_.publish(temp_array);
    }
    else
    {
      ROS_DEBUG("sdh not initialized");
    }
    // publishing diagnotic messages
    diagnostic_msgs::DiagnosticArray diagnostics;
    diagnostics.status.resize(1);
    // set data to diagnostics
    if (isError_)
    {
      diagnostics.status[0].level = 2;
      diagnostics.status[0].name = "schunk_powercube_chain";
      diagnostics.status[0].message = "one or more drives are in Error mode";
    }
    else
    {
      if (isInitialized_)
      {
        diagnostics.status[0].level = 0;
        diagnostics.status[0].name = nh_.getNamespace();  // "schunk_powercube_chain";
        diagnostics.status[0].message = "sdh initialized and running";
      }
      else
      {
        diagnostics.status[0].level = 1;
        diagnostics.status[0].name = nh_.getNamespace();  // "schunk_powercube_chain";
        diagnostics.status[0].message = "sdh not initialized";
      }
    }
    // publish diagnostic message
    topicPub_Diagnostics_.publish(diagnostics);
  }
};

const std::vector<std::string> SdhNode::temperature_names_ = {
    "root",
    "proximal_finger_1", "distal_finger_1",
    "proximal_finger_2", "distal_finger_2",
    "proximal_finger_3", "distal_finger_3",
    "controller", "pcb"
};
// SdhNode

/*!
 * \brief Main loop of ROS node.
 *
 * Running with a specific frequency defined by loop_rate.
 */
int main(int argc, char** argv)
{
  // initialize ROS, spezify name of node
  ros::init(argc, argv, "schunk_sdh");

  SdhNode sdh_node(ros::this_node::getName() + "/follow_joint_trajectory");
  if (!sdh_node.init())
    return 0;

  ROS_INFO("...sdh node running...");

  double frequency;
  if (sdh_node.nh_.hasParam("frequency"))
  {
    sdh_node.nh_.getParam("frequency", frequency);
  }
  else
  {
    frequency = 50;  // Hz
    ROS_WARN("Parameter frequency not available, setting to default value: %f Hz", frequency);
  }

  // sleep(1);
  ros::Rate loop_rate(frequency);  // Hz
  while (sdh_node.nh_.ok())
  {
    // publish JointState
    sdh_node.updateSdh();

    // sleep and waiting for messages, callbacks
    ros::spinOnce();
    loop_rate.sleep();
  }

  return 0;
}

