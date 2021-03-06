/*
 * WSG 50 ROS NODE
 * Copyright (c) 2012, Robotnik Automation, SLL
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Robotnik Automation, SLL. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * \author Marc Benetó (mbeneto@robotnik.es)
 * \brief WSG-50 ROS driver.
 */


//------------------------------------------------------------------------
// Includes
//------------------------------------------------------------------------


#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <thread>
#include <chrono>
#include <mutex>


#include "wsg50/common.h"
#include "wsg50/cmd.h"
#include "wsg50/msg.h"
#include "wsg50/tcp.h"
#include "wsg50/functions.h"

#include <ros/ros.h>
#include "std_msgs/String.h"
#include "std_srvs/Empty.h"
#include "wsg50_common/Status.h"
#include "wsg50_common/Move.h"
#include "wsg50_common/Conf.h"
#include "wsg50_common/Incr.h"
#include "wsg50_common/Cmd.h"
#include "dnb_msgs/ComponentStatus.h"

#include "sensor_msgs/JointState.h"
#include "std_msgs/Float32.h"
#include "std_msgs/Bool.h"


//------------------------------------------------------------------------
// Local macros
//------------------------------------------------------------------------


//------------------------------------------------------------------------
// Typedefs, enums, structs
//------------------------------------------------------------------------

#define GRIPPER_MIN_OPEN 0.0

//------------------------------------------------------------------------
// Global variables
//------------------------------------------------------------------------

float increment;
bool objectGraspped;
bool in_motion;
unsigned char last_cmd_id;
bool stop_called;

const float timeout_commands = 30.0;

ros::Publisher g_pub_state, g_pub_joint, g_pub_moving;
ros::Publisher component_status;
int g_timer_cnt = 0, g_size;
bool g_ismoving = false, g_mode_script = false, g_mode_periodic = false, g_mode_polling = false;
float g_goal_position = NAN, g_goal_speed = NAN, g_speed = 10.0;


//------------------------------------------------------------------------
// Unit testing
//------------------------------------------------------------------------


//------------------------------------------------------------------------
// Local function prototypes
//------------------------------------------------------------------------


//------------------------------------------------------------------------
// Function implementation
//------------------------------------------------------------------------
void publish_status_and_joint_states(gripper_response info) {
    // ==== Status msg ====
    wsg50_common::Status status_msg;
    status_msg.status = info.state_text;
    status_msg.width = info.position;
    status_msg.speed = info.speed;
    status_msg.acc = info.acceleration;
    status_msg.force = info.f_motor;
    status_msg.force_finger0 = info.f_finger0;
    status_msg.force_finger1 = info.f_finger1;

    g_pub_state.publish(status_msg);

    // ==== Joint state msg ====
    sensor_msgs::JointState joint_states;
    joint_states.header.stamp = ros::Time::now();;
    joint_states.header.frame_id = "";//"wsg50_gripper_base_link";
    joint_states.name.push_back("wsg50_finger_left_joint");
    joint_states.position.resize(1);

    joint_states.position[0] = info.position/2000.0;
    joint_states.velocity.resize(1);
    joint_states.velocity[0] = info.speed/1000.0;
    joint_states.effort.resize(1);
    joint_states.effort[0] = info.f_motor;

    g_pub_joint.publish(joint_states);
}

bool moveSrv(wsg50_common::Move::Request &req, wsg50_common::Move::Response &res) {

    if (in_motion){
        ROS_WARN("Another Motion Control service is already running. Try again later!");
        res.error = E_ALREADY_RUNNING;
        return true;
    }

    if (req.width < 0.0 || req.width > g_size) {
        ROS_ERROR("Imposible to move to this position. (Width values: [0.0 - %d] ", g_size);
        res.error = E_RANGE_ERROR;
        return false;
    }

    if ( req.speed < 0.0 || req.speed > 420.0 ) {
        ROS_WARN("Speed values are outside the gripper's physical limits ([0.1 - 420.0])  Using clamped values.");
    }

    // Moving asnchronously while spinning to check for stop command
    if (move_async(req.width, req.speed, false) == 0) {
        in_motion = true;
        last_cmd_id = 0x21;

        status_t status;
        int msg_available = 0; // 0 when no msg available, 1 when msg is available and correct, -1 on error

        ros::Time begin = ros::Time::now();
        do {

            msg_available = recv_ack(0x21, &status);
            if (msg_available == 1 && status == E_CMD_PENDING) ROS_INFO("Moving to %f position at %f mm/s.", req.width, req.speed);
            ros::spinOnce();

            if (stop_called) {
                res.error = E_CMD_ABORTED;
                stop_called = false;
                in_motion = false;
                return true;
            }

            // After 10 seconds, exit
            if ((ros::Time::now() - begin).toSec() > timeout_commands){
                //close( conn.sock );
                quit("Failed to read data from TCP socket");
            }

        }
        while (msg_available == 0 || (msg_available == 1 && status == E_CMD_PENDING));

        ros::Duration(0.1).sleep(); // A small delay to fix the problem with hw-rev. 2 grippers

        in_motion = false;

        res.error = msg_available == -1? 255 : status;
        if (res.error == 0) {
            ROS_INFO("Target position reached");
        } else {
            ROS_ERROR("Failed while moving : %s", status_to_str(status));
        }
    }else {
        res.error = 255;
    }

    return true;
}

bool graspSrv(wsg50_common::Move::Request &req, wsg50_common::Move::Response &res){
    if (in_motion){
        ROS_WARN("Another Motion Control service is already running. Try again later!");
        res.error = E_ALREADY_RUNNING;
        return true;
    }

    if (req.width < 0.0 || req.width > g_size) {
        ROS_ERROR("Imposible to move to this position. (Width values: [0.0 - %d] ", g_size);
        res.error = E_RANGE_ERROR;
        return false;
    }

    if ( req.speed < 0.0 || req.speed > 420.0 ) {
        ROS_WARN("Speed values are outside the gripper's physical limits ([0.1 - 420.0])  Using clamped values.");
    }

    // Moving asnchronously while spinning to check for stop command
    if (grasp_async(req.width, req.speed) == 0) {        
        in_motion = true;
        last_cmd_id = 0x25;

        status_t status;
        int msg_available = 0; // 0 when no msg available, 1 when msg is available and correct, -1 on error

        ros::Time begin = ros::Time::now();
        do {
            msg_available = recv_ack(0x25, &status);
            if (msg_available == 1 && status == E_CMD_PENDING) ROS_INFO("Grasping object at %f with %f mm/s.", req.width, req.speed);
            ros::spinOnce();
            if (stop_called) {
                res.error = E_CMD_ABORTED;
                stop_called = false;
                in_motion = false;
                return true;
            }
            // After 10 seconds, exit
            if ((ros::Time::now() - begin).toSec() > timeout_commands){
                //close( conn.sock );
                quit("Failed to read data from TCP socket");
            }
        }
        while (msg_available == 0 || (msg_available == 1 && status == E_CMD_PENDING));

        ros::Duration(0.1).sleep(); // A small delay to fix the problem with hw-rev. 2 grippers

        in_motion = false;

        res.error = msg_available == -1? 255 : status;
        if (res.error == 0) {
            ROS_INFO("Object grasped correctly.");
            objectGraspped=true;
        } else {
            ROS_ERROR("Failed while grasping object : %s", status_to_str(status));
        }
    }else {
        res.error = 255;
    }

    return true;
}

bool incrementSrv(wsg50_common::Incr::Request &req, wsg50_common::Incr::Response &res) {
    if (in_motion){
        ROS_WARN("Another Motion Control service is already running. Try again later!");
        res.error = E_ALREADY_RUNNING;
        return true;
    }

    float currentWidth, nextWidth, speed; 
    if (req.direction == "open") {  
        currentWidth = getOpening();
        nextWidth = currentWidth + req.increment;
        nextWidth = nextWidth >= g_size? g_size : nextWidth;
        speed = nextWidth >= g_size? 1 : 20;
    }
    else if (req.direction == "close") {
        currentWidth = getOpening();  
        nextWidth = currentWidth - req.increment;
        nextWidth = nextWidth <= GRIPPER_MIN_OPEN? GRIPPER_MIN_OPEN : nextWidth;
        speed = nextWidth <= GRIPPER_MIN_OPEN? 1 : 20;
    }

    if (req.direction == "open" || req.direction == "close") {
        // Incremental moving asnchronously while spinning to check for stop command
        if (move_async(nextWidth, speed, true) == 0) {   
            in_motion = true;
            last_cmd_id = 0x21;


            status_t status;
            int msg_available = 0; // 0 when no msg available, 1 when msg is available and correct, -1 on error
            ros::Time begin = ros::Time::now();
            do {
                msg_available = recv_ack(0x21, &status);
                if (msg_available == 1 && status == E_CMD_PENDING) {
                    ROS_INFO("Incremental %sing of %f mm - CurrWidth: %f - NxtWidth: %f", req.direction.c_str(), req.increment, currentWidth, nextWidth);            
                }
                ros::spinOnce();
                if (stop_called) {
                    res.error = E_CMD_ABORTED;
                    stop_called = false;
                    in_motion = false;
                    return true;
                }
                // After 10 seconds, exit
                if ((ros::Time::now() - begin).toSec() > timeout_commands){
                    //close( conn.sock );
                    quit("Failed to read data from TCP socket");
                }
            }
            while (msg_available == 0 || (msg_available == 1 && status == E_CMD_PENDING));

            ros::Duration(0.1).sleep(); // A small delay to fix the problem with hw-rev. 2 grippers

            in_motion = false;

            res.error = msg_available == -1? 255 : status;
            if (res.error == 0) {
                ROS_INFO("Incremental %s done", req.direction.c_str());
            } else {
                ROS_ERROR("Failed while move incrementally : %s", status_to_str(status));
            }
        }
        else {
            res.error = 255;
        }
    }
    
    return true;
}

bool releaseSrv(wsg50_common::Move::Request &req, wsg50_common::Move::Response &res) {
    if (in_motion){
        ROS_WARN("Another Motion Control service is already running. Try again later!");
        res.error = E_ALREADY_RUNNING;
        return true;
    }

    if (req.width < 0.0 || req.width > g_size) {
        ROS_ERROR("Imposible to move to this position. (Width values: [0.0 - %d] ", g_size);
        res.error = E_RANGE_ERROR;
        return false;
    }

    if ( req.speed < 0.0 || req.speed > 420.0 ) {
        ROS_WARN("Speed values are outside the gripper's physical limits ([0.1 - 420.0])  Using clamped values.");
    }

    // Releasing asnchronously while spinning to check for stop command
    if (release_async(req.width, req.speed) == 0) {
        in_motion = true;
        last_cmd_id = 0x26;

        status_t status;
        int msg_available = 0; // 0 when no msg available, 1 when msg is available and correct, -1 on error
        ros::Time begin = ros::Time::now();
        do {
            msg_available = recv_ack(0x26, &status);
            if (msg_available == 1 && status == E_CMD_PENDING) ROS_INFO("Releasing to %f position at %f mm/s.", req.width, req.speed);
            ros::spinOnce();
            if (stop_called) {
                res.error = E_CMD_ABORTED;
                stop_called = false;
                in_motion = false;
                return true;
            }
            // After 10 seconds, exit
            if ((ros::Time::now() - begin).toSec() > timeout_commands){
                //close( conn.sock );
                quit("Failed to read data from TCP socket");
            }
        }
        while (msg_available == 0 || (msg_available == 1 && status == E_CMD_PENDING));

        ros::Duration(0.1).sleep(); // A small delay to fix the problem with hw-rev. 2 grippers

        in_motion = false;

        res.error = msg_available == -1? 255 : status;
        if (res.error == 0) {
            objectGraspped = false;
            ROS_INFO("Object released correctly.");
        } else {
            ROS_ERROR("Failed to release object : %s", status_to_str(status));
        }
    }else {
        res.error = 255;
    }
    return true;
}

bool homingSrv(std_srvs::Empty::Request &req, std_srvs::Empty::Request &res) {
    if (in_motion){
        ROS_WARN("Another Motion Control service is already running. Try again later!");
        return false;
    }

    // Homing asnchronously while spinning to check for stop command
    if (homing_async() == 0) {
        in_motion = true;
        last_cmd_id = 0x20;
        status_t status;
        int msg_available = 0; // 0 when no msg available, 1 when msg is available and correct, -1 on error
        ros::Time begin = ros::Time::now();
        do {
            msg_available = recv_ack(0x20, &status);
            if (msg_available == 1 && status == E_CMD_PENDING) ROS_INFO("Homing...");
            // After 10 seconds, exit
            if ((ros::Time::now() - begin).toSec() > timeout_commands){
                //close( conn.sock );
                quit("Failed to read data from TCP socket");
            }
        }
        while (msg_available == 0 || (msg_available == 1 && status == E_CMD_PENDING));

        ros::Duration(0.1).sleep(); // A small delay to fix the problem with hw-rev. 2 grippers

        in_motion = false;

        if (msg_available == -1) return false;

        if (msg_available == 1 && status == E_SUCCESS) {
            ROS_INFO("Home position reached.");
        } else {
            ROS_ERROR("Failed to reach home position : %s", status_to_str(status));
            return false;
        }
    }

    return true;
}

bool stopSrv(std_srvs::Empty::Request &req, std_srvs::Empty::Request &res) {
    ROS_WARN("Stop!");

    if (in_motion) {
        status_t status;
        // In practice, if a homing command was running, the gripper will not send response of the stop message immediately
        // & hence response should be ignored
        stop(last_cmd_id == 0x20); 
        ROS_WARN("Waiting for the last motion control command");
        while (recv_ack(last_cmd_id, &status) == 0);
        stop_called = true; // this will abort the last motion control service once stop returns 
    }
    else {
        stop();
    }

    ROS_WARN("Stopped.");
    return true;
}

// bool stopSrv(std_srvs::Empty::Request &req, std_srvs::Empty::Request &res) {
//     ROS_WARN("Stop!");
    
//     stop();
//     // If a motion control command was interrupted, wait for its response
//     if (in_motion) {
//         ROS_WARN("Waiting for the last motion control command");
//         status_t status;
//         if (last_cmd_id == 0x20) {
//             while (recv_ack(0x22, &status) == 0);
//         } else {
//             while (recv_ack(last_cmd_id, &status) == 0);
//         }
//         stop_called = true; // this will abort the last motion control service once stop returns 
//     }
//     else {
//         stop();
//     }

//     ROS_WARN("Stopped.");
//     return true;
// }


bool setAccSrv(wsg50_common::Conf::Request &req, wsg50_common::Conf::Response &res)
{
    setAcceleration(req.val);
    return true;
}

bool setForceSrv(wsg50_common::Conf::Request &req, wsg50_common::Conf::Response &res)
{
    setGraspingForceLimit(req.val);
    return true;
}

bool ackSrv(std_srvs::Empty::Request &req, std_srvs::Empty::Request &res)
{
    ack_fault();
    return true;
}

/** \brief Callback for goal_position topic (in appropriate modes) */
void position_cb(const wsg50_common::Cmd::ConstPtr& msg)
{
    g_speed = msg->speed;
    g_goal_position = msg->pos;
    // timer_cb() will send command to gripper

    if (g_mode_periodic) {
        // Send command to gripper without waiting for a response
        // read_thread() handles responses
        // read/write may be simultaneous, therefore no mutex
        stop(true);
        if (move(g_goal_position, g_speed, false, true) != 0)
            ROS_ERROR("Failed to send MOVE command");
    }
}

/** \brief Callback for goal_speed topic (in appropriate modes) */
void speed_cb(const std_msgs::Float32::ConstPtr& msg)
{
    g_goal_speed = msg->data;
    g_speed = msg->data;
    // timer_cb() will send command to gripper
}

/** \brief Loop for state polling in modes script and polling. Also sends command in script mode. */
void timer_cb(const ros::TimerEvent& ev)
{
    // ==== Get state values by built-in commands ====
    gripper_response info;
    info.acceleration = 0.0;
    info.speed = 0.0;

    if (g_mode_polling && !in_motion) {
        const char * state = systemState();
        if (!state)
            return;
        info.state_text = std::string(state);
        info.position = getOpening();
        info.acceleration = getAcceleration();
        info.f_motor = getForce();//getGraspingForce();

    } else if (g_mode_script) {
        // ==== Call custom measure-and-move command ====
        int res = 0;
        if (!std::isnan(g_goal_position)) {
            ROS_INFO("Position command: pos=%5.1f, speed=%5.1f", g_goal_position, g_speed);
            res = script_measure_move(1, g_goal_position, g_speed, info);
        } else if (!std::isnan(g_goal_speed)) {
            ROS_INFO("Velocity command: speed=%5.1f", g_goal_speed);
            res = script_measure_move(2, 0, g_goal_speed, info);
        } else
            res = script_measure_move(0, 0, 0, info);
        if (!std::isnan(g_goal_position))
            g_goal_position = NAN;
        if (!std::isnan(g_goal_speed))
            g_goal_speed = NAN;

        if (!res) {
            ROS_ERROR("Measure-and-move command failed");
            return;
        }

        // ==== Moving msg ====
        if (g_ismoving != info.ismoving) {
            std_msgs::Bool moving_msg;
            moving_msg.data = info.ismoving;
            g_pub_moving.publish(moving_msg);
            g_ismoving = info.ismoving;
        }
    } else
        return;

    publish_status_and_joint_states(info);

    // printf("Timer, last duration: %6.1f\n", ev.profile.last_duration.toSec() * 1000.0);
}

/** \brief Reads gripper responses in auto_update mode. The gripper pushes state messages in regular intervals. */
void read_thread(int interval_ms)
{
    ROS_INFO("Thread started");

    status_t status;
    int res;
    bool pub_state = false;

    double rate_exp = 1000.0 / (double)interval_ms;
    std::string names[3] = { "opening", "speed", "force" };

    // Prepare messages
    gripper_response info;
    info.state_text = "UNKNOWN";

    // Request automatic updates (error checking is done below)
    getOpening(interval_ms);
    getSpeed(interval_ms);
    getForce(interval_ms);


    msg_t msg;
    msg.id = 0;
    msg.data = 0;
    msg.len = 0;
    int cnt[3] = {0,0,0};
    auto time_start = std::chrono::system_clock::now();


    while (g_mode_periodic) {
        // Receive gripper response
        msg_free(&msg);
        res = msg_receive( &msg );
        if (res < 0 || msg.len < 2) {
            ROS_ERROR("Gripper response failure: too short");
            continue;
        }

        float val = 0.0;
        status = cmd_get_response_status(msg.data);

        // Decode float for opening/speed/force
        if (msg.id >= 0x43 && msg.id <= 0x45 && msg.len == 6) {
            if (status != E_SUCCESS) {
                ROS_ERROR("Gripper response failure for opening/speed/force\n");
                continue;
            }
            val = convert(&msg.data[2]);
        }

        // Handle response types
        int motion = -1;
        switch (msg.id) {
        /*** Opening ***/
        case 0x43:
            info.position = val;
            pub_state = true;
            cnt[0]++;
            break;

        /*** Speed ***/
        case 0x44:
            info.speed = val;
            cnt[1]++;
            break;

        /*** Force ***/
        case 0x45:
            info.f_motor = val;
            cnt[2]++;
            break;

        /*** Move ***/
        // Move commands are sent from outside this thread
        case 0x21:
            if (status == E_SUCCESS) {
                ROS_INFO("Position reached");
                motion = 0;
            } else if (status == E_AXIS_BLOCKED) {
                ROS_INFO("Axis blocked");
                motion = 0;
            } else if (status == E_CMD_PENDING) {
                ROS_INFO("Movement started");
                motion = 1;
            } else if (status == E_ALREADY_RUNNING) {
                ROS_INFO("Movement error: already running");
            } else if (status == E_CMD_ABORTED) {
                ROS_INFO("Movement aborted");
                motion = 0;
            } else {
                ROS_INFO("Movement error");
                motion = 0;
            }
            break;

        /*** Stop ***/
        // Stop commands are sent from outside this thread
        case 0x22:
            // Stop command; nothing to do
            break;
        default:
            ROS_INFO("Received unknown respone 0x%02x (%2dB)\n", msg.id, msg.len);
        }

        // ***** PUBLISH motion message
        if (motion == 0 || motion == 1) {
            std_msgs::Bool moving_msg;
            moving_msg.data = motion;
            g_pub_moving.publish(moving_msg);
            g_ismoving = motion;
        }

        // ***** PUBLISH state message & joint message
        if (pub_state) {
            pub_state = false;
            publish_status_and_joint_states(info);
        }

        // Check # of received messages regularly
        std::chrono::duration<float> t = std::chrono::system_clock::now() - time_start;
        double t_ = t.count();
        if (t_ > 5.0) {
            time_start = std::chrono::system_clock::now();
            //printf("Infos for %5.1fHz, %5.1fHz, %5.1fHz\n", (double)cnt[0]/t_, (double)cnt[1]/t_, (double)cnt[2]/t_);

            std::string info = "Rates for ";
            for (int i=0; i<3; i++) {
                double rate_is = (double)cnt[i]/t_;
                info += names[i] + ": " + std::to_string((int)rate_is) + "Hz, ";
                if (rate_is == 0.0)
                    ROS_ERROR("Did not receive data for %s", names[i].c_str());
            }
            ROS_DEBUG_STREAM((info + " expected: " + std::to_string((int)rate_exp) + "Hz").c_str());
            cnt[0] = 0;
            cnt[1] = 0;
            cnt[2] = 0;
        }


    }

    // Disable automatic updates
    // TODO: The functions will receive an unexpected response
    getOpening(0);
    getSpeed(0);
    getForce(0);

    ROS_INFO("Thread ended");
}

void sigint_handler(int sig) {
    ROS_INFO("Exiting...");
    g_mode_periodic = false;
    g_mode_script = false;
    g_mode_polling = false;
    ros::shutdown();
}

/**
 * The main function
 */

int main( int argc, char **argv )
{
    ros::init(argc, argv, "wsg50");
    ros::NodeHandle nh("~");
    signal(SIGINT, sigint_handler);
    in_motion = false; 
    stop_called = false;

    component_status = nh.advertise<dnb_msgs::ComponentStatus>("component/status", 1, true);
    dnb_msgs::ComponentStatus cstatus_msg;
    std::string ip, protocol, com_mode, serial_port;
    int size, port, local_port, serial_baudrate;
    double rate, grasping_force;
    bool use_udp = false;

    nh.param("size", size, 210);
    nh.param("ip", ip, std::string("192.168.1.20"));
    nh.param("port", port, 1000);
    nh.param("local_port", local_port, 1501);
    nh.param("serial_port", serial_port, std::string("/dev/ttyS1"));
    nh.param("serial_baudrate", serial_baudrate, 115200);
    nh.param("protocol", protocol, std::string("serial"));
    nh.param("com_mode", com_mode, std::string(""));
    nh.param("rate", rate, 5.0); // With custom script, up to 30Hz are possible
    nh.param("grasping_force", grasping_force, 0.0);

    if (size != 210 && size != 110) {
        ROS_WARN("Gripper size: %d is invalid. Using default size of 210 mm.", size);
        g_size = 210;
    }
    else {
        g_size = size;
    }

    if (com_mode == "script")
        g_mode_script = true;
    else if (com_mode == "auto_update")
        g_mode_periodic = true;
    else {
        com_mode = "polling";
        g_mode_polling = true;
    }

    // Connect to device using Serial/TCP/UDPđ
    int res_con = -1;
    if (protocol == "serial") {
        res_con = cmd_connect_serial(serial_port.c_str(), serial_baudrate);
        ROS_INFO("Connecting to %s:%d (%s); communication mode: %s ...", serial_port.c_str(), serial_baudrate, protocol.c_str(), com_mode.c_str());
    }
    else if (protocol == "udp") {
        res_con = cmd_connect_udp(local_port, ip.c_str(), port );
        ROS_INFO("Connecting to %s:%d (%s); communication mode: %s ...", ip.c_str(), port, protocol.c_str(), com_mode.c_str());
    }
    else if (protocol == "tcp") {
        res_con = cmd_connect_tcp( ip.c_str(), port );
        ROS_INFO("Connecting to %s:%d (%s); communication mode: %s ...", ip.c_str(), port, protocol.c_str(), com_mode.c_str());
    }
    else {
        ROS_ERROR("UNKNOWN protocol!");
    }

    // check if connection was successful
    if (res_con == 0 ) {
        ROS_INFO("Gripper connection established");
        //pub

        // Services
        ros::ServiceServer moveSS, graspSS, releaseSS, homingSS, stopSS, ackSS, incrementSS, setAccSS, setForceSS;

        if (g_mode_script || g_mode_polling) {
            moveSS = nh.advertiseService("move", moveSrv);
            graspSS = nh.advertiseService("grasp", graspSrv);
            releaseSS = nh.advertiseService("release", releaseSrv);
            homingSS = nh.advertiseService("homing", homingSrv);
            stopSS = nh.advertiseService("stop", stopSrv);
            ackSS = nh.advertiseService("ack", ackSrv);
            incrementSS = nh.advertiseService("move_incrementally", incrementSrv);

            setAccSS = nh.advertiseService("set_acceleration", setAccSrv);
            setForceSS = nh.advertiseService("set_force", setForceSrv);
        }

        // Subscriber
        ros::Subscriber sub_position, sub_speed;
        if (g_mode_script || g_mode_periodic)
            sub_position = nh.subscribe("goal_position", 5, position_cb);
        if (g_mode_script)
            sub_speed = nh.subscribe("goal_speed", 5, speed_cb);

        // Publisher
        g_pub_state = nh.advertise<wsg50_common::Status>("status", 1000);
        g_pub_joint = nh.advertise<sensor_msgs::JointState>("/joint_states", 10);

        if (g_mode_script || g_mode_periodic)
            g_pub_moving = nh.advertise<std_msgs::Bool>("moving", 10);

        sleep(1);

        ack_fault();

        sleep(1);

        ROS_INFO("Ready to use, homing now...");
        homing();

        // Set component status to running
        cstatus_msg.status_id = 2;
        cstatus_msg.status_msg = "WSG50 was homed and is running.";
        component_status.publish(cstatus_msg);

        if (grasping_force > 0.0) {
            ROS_INFO("Setting grasping force limit to %5.1f", grasping_force);
            setGraspingForceLimit(grasping_force);
        }

        ROS_INFO("Init done. Starting timer/thread with target rate %.1f.", rate);
        std::thread th;
        ros::Timer tmr;
        if (g_mode_polling || g_mode_script)
            tmr = nh.createTimer(ros::Duration(1.0/rate), timer_cb);
        if (g_mode_periodic)
            th = std::thread(read_thread, (int)(1000.0/rate));

        sleep(5);

        ros::spin();

    } else {
        ROS_ERROR("Unable to connect, please check the port and address used.");

        // Set component status to error
        cstatus_msg.status_id = 4;
        cstatus_msg.status_msg = "Unable to connect, please check the port and address used.";
        component_status.publish(cstatus_msg);
    }

    // Set component status to running
    cstatus_msg.status_id = 1;
    cstatus_msg.status_msg = "Driver was stopped.";
    component_status.publish(cstatus_msg);

    ROS_INFO("Exiting...");
    g_mode_periodic = false;
    g_mode_script = false;
    g_mode_polling = false;
    sleep(1);
    cmd_disconnect();

    return 0;

}


//------------------------------------------------------------------------
// Testing functions
//------------------------------------------------------------------------
