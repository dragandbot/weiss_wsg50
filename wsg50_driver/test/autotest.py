#!/usr/bin/env python

PKG = 'wsg50_driver'
NAME = 'autotest'

import sys
import json                 # Json API
import rospy                # ROS API
import rostest				# ROS specific testing library
import unittest				# Python generic testing library used by ROS
import time
from std_srvs.srv import *
from wsg50_common.srv import *     # For loading service


class TestGripper(unittest.TestCase):
	# Each test case is individually defined as a function

	# # Test Moving
	# def test_moving(self):
	# 	rospy.wait_for_service('/wsg50_driver/move')
	# 	srv_move = rospy.ServiceProxy('/wsg50_driver/move', Move)
	# 	res = srv_move(25, 15)
	# 	assert(res.error == 0)

	# # Test Homing
	# def test_homing(self):
	# 	rospy.wait_for_service('/wsg50_driver/homing')
	# 	srv_home = rospy.ServiceProxy('/wsg50_driver/homing', Empty)
	# 	res = srv_home()

	# Test Delta/Icremental Moving - close
	def test_continuous_close(self):
		rospy.wait_for_service('/wsg50_driver/homing')
		srv_home = rospy.ServiceProxy('/wsg50_driver/homing', Empty)
		res = srv_home()

		rospy.wait_for_service('/wsg50_driver/move_incrementally')
		srv_delta_move = rospy.ServiceProxy('/wsg50_driver/move_incrementally', Incr)
		for i in range(0, 4):
			res = srv_delta_move('close', 15)
			assert(res.error == 0)

	# Test Delta/Icremental Moving - open
	def test_continuous_open(self):		
		rospy.wait_for_service('/wsg50_driver/move')
		srv_move = rospy.ServiceProxy('/wsg50_driver/move', Move)
		res = srv_move(0, 20)

		rospy.wait_for_service('/wsg50_driver/move_incrementally')
		srv_delta_move = rospy.ServiceProxy('/wsg50_driver/move_incrementally', Incr)
		for i in range(0, 4):
			res = srv_delta_move('open', 15)
			assert(res.error == 0)

	# Test Delta/Icremental Moving - open
	def test_close_open(self):		
		rospy.wait_for_service('/wsg50_driver/homing')
		srv_home = rospy.ServiceProxy('/wsg50_driver/homing', Empty)
		res = srv_home()

		rospy.wait_for_service('/wsg50_driver/move_incrementally')
		srv_delta_move = rospy.ServiceProxy('/wsg50_driver/move_incrementally', Incr)

		res = srv_delta_move('close', 15)
		assert(res.error == 0)

		res = srv_delta_move('close', 15)
		assert(res.error == 0)

		res = srv_delta_move('open', 15)
		assert(res.error == 0)

		res = srv_delta_move('open', 15)
		assert(res.error == 0)

		res = srv_delta_move('close', 15)
		assert(res.error == 0)

	# Test Grasping/Releasing with no object to grasp
	def test_grasp_release(self):
		rospy.wait_for_service('/wsg50_driver/homing')
		srv_home = rospy.ServiceProxy('/wsg50_driver/homing', Empty)
		res = srv_home()

		rospy.wait_for_service('/wsg50_driver/grasp')
		srv_grasp = rospy.ServiceProxy('/wsg50_driver/grasp', Move)
		res = srv_grasp(30, 15)
		assert(res.error != 0)

		rospy.wait_for_service('/wsg50_driver/release')
		srv_release = rospy.ServiceProxy('/wsg50_driver/release', Move)
		res = srv_release(100, 15)
		assert(res.error == 0)
	
	# # Test Grasping
	# def test_grasping(self):
	# 	rospy.wait_for_service('/wsg50_driver/grasp')
	# 	srv_grasp = rospy.ServiceProxy('/wsg50_driver/grasp', Move)
	# 	res = srv_grasp(30, 15)
	# 	assert(res.error != 0)

	# # Test Releasing
	# def test_releasing(self):
	# 	rospy.wait_for_service('/wsg50_driver/release')
	# 	srv_release = rospy.ServiceProxy('/wsg50_driver/release', Move)
	# 	res = srv_release(100, 15)
	# 	assert(res.error != 0)

	# --------------------------------------------
	# Extend it with new tests here
	# Please create programs in testing_tools.py file to keep this .py small
	# --------------------------------------------

if __name__ == '__main__':
	rospy.init_node('wsg50_driver_autotest') # Needed for calling the action, without init it fails
	rostest.rosrun(PKG, NAME, TestGripper, sys.argv)