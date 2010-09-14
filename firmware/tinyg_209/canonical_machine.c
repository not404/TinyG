/*
 * canonical_machine.c - rs274/ngc canonical machine for cartesian robot.
 * Part of TinyG
 *
 * This code is a loose implementation of Kramer, Proctor and Messina's
 * canonical machining functions as described in the NIST RS274/NGC v3
 * Sections of this code are adapted from Simen Svale Skogsrud's grbl
 * 
 * Copyright (c) 2010 Alden S Hart, Jr.
 * Portions copyright (c) 2009 Simen Svale Skogsrud
 *
 * TinyG is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software 
 * Foundation, either version 3 of the License, or (at your option) any later 
 * version.
 *
 * TinyG is distributed in the hope that it will be useful, but WITHOUT ANY 
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS 
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for details.
 *
 * You should have received a copy of the GNU General Public License along 
 * with TinyG  If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <stdio.h>
#include <avr/pgmspace.h>			// needed for exception strings

#include "tinyg.h"
#include "gcode.h"
#include "config.h"
#include "motion_control.h"
#include "canonical_machine.h"
#include "spindle.h"

/* data structures (see notes in gcode.c) */
struct GCodeModel gm;		// gcode model
static uint8_t cm_status;

/* useful macros */
#ifndef ZERO_MODEL_STATE
#define ZERO_MODEL_STATE(g) memset(g, 0, sizeof(struct GCodeModel))
#endif

static double _to_millimeters(double value);
static double _theta(double x, double y);
static int _gc_compute_radius_arc(void);
static int _gc_compute_center_arc(void);

/*************************************************************************
 *
 * CANONICAL MACHINING FUNCTIONS
 *
 *	Values are passed in pre-unit_converted state
 *	All operations occur on gm (current model state)
 *
 ************************************************************************/

/*
 * Getters
 *
 * cm_get_position() - return position from the gm struct into gn struct form
 * cm_get_next_action() - get next_action from the gm struct
 * cm_get_motion_mode() - get motion mode from the gm struct
 */

inline double cm_get_position(uint8_t axis) 
{
	return (gm.inches_mode ? (gm.position[axis] / MM_PER_INCH) : gm.position[axis]);
}

inline uint8_t cm_get_next_action() { return gm.next_action; }
inline uint8_t cm_get_motion_mode() { return gm.motion_mode; }

/*
 * Setters
 *
 * cm_set_positions()	- set all XYZ positions
 * cm_set_targets()		- set all XYZ targets
 * cm_set_offsets()		- set all IJK offsets
 * cm_set_position() 	- set one XYZ position
 * cm_set_target()		- set one XYZ target
 * cm_set_offset()		- set one IJK offset
 * cm_set_radius()		- set radius value
 *
 *	Input coordinates are in native block formats;
 *	i.e. they are not unit adjusted or otherwise pre-processed
 */

inline void cm_set_positions(double x, double y, double z) 
{ 
	gm.position[X] = _to_millimeters(x);
	gm.position[Y] = _to_millimeters(y);
	gm.position[Z] = _to_millimeters(z);
}

inline void cm_set_targets(double x, double y, double z) 
{ 
	gm.target[X] = _to_millimeters(x);
	gm.target[Y] = _to_millimeters(y);
	gm.target[Z] = _to_millimeters(z);
}

inline void cm_set_offsets(double i, double j, double k) 
{ 
	gm.offset[0] = _to_millimeters(i);
	gm.offset[1] = _to_millimeters(j);
	gm.offset[2] = _to_millimeters(k);
}

inline void cm_set_position(uint8_t axis, double value) 
{ 
	gm.position[axis] = _to_millimeters(value);
}

inline void cm_set_target(uint8_t axis, double value) 
{ 
	gm.target[axis] = _to_millimeters(value);
}

inline void cm_set_offset(uint8_t axis, double value) 
{ 
	gm.offset[axis] = _to_millimeters(value);
}

inline void cm_set_radius(double r) 
{ 
	gm.radius = _to_millimeters(r);
}

/* 
 * Helper routines
 *
 * _to_millimeters()
 * _theta()
 *
 */

inline double _to_millimeters(double value)
{
	return(gm.inches_mode ? (value * MM_PER_INCH) : value);
}

/* 
 * _theta(double x, double y)
 *
 *	Find the angle in radians of deviance from the positive y axis. 
 *	negative angles to the left of y-axis, positive to the right.
 */

static double _theta(double x, double y)
{
	double theta = atan(x/fabs(y));

	if (y>0) {
		return(theta);
	} else {
		if (theta>0) 
	    {
			return(M_PI-theta);
    	} else {
			return(-M_PI-theta);
		}
	}
}

/*--- CANONICAL MACHINING FUNCTIONS ---*/

/* 
 * Initialization and Termination (4.3.2)
 *
 * cm_init_canon() 
 */


void cm_init_canon()
{
	ZERO_MODEL_STATE(&gm);	
	cm_select_plane(CANON_PLANE_XY);		// default planes, 0, 1 and 2
	gm.seek_rate = cfg.max_seek_rate;		// in mm/minute
	gm.max_seek_rate = cfg.max_seek_rate;	// in mm/minute
	gm.max_feed_rate = cfg.max_feed_rate;	// in mm/minute
//	gm.inches_mode = TRUE;					// default to inches (G20)
	gm.absolute_mode = TRUE;				// default to absolute mode (G90)
}

/* 
 * Representation (4.3.3)
 *
 * cm_select_plane() - select axis plane Defaults to XY on erroneous specification
 * cm_set_origin_offsets() - G92
 * cm_use_length_units()  - G20, G21
 * cm_set_distance_mode() - G90, G91
 */

uint8_t cm_select_plane(uint8_t plane) 
{
	if (plane == CANON_PLANE_YZ) {
		gm.plane_axis_0 = Y;
		gm.plane_axis_1 = Z;
		gm.plane_axis_2 = X;
	} else if (plane == CANON_PLANE_XZ) {
		gm.plane_axis_0 = X;
		gm.plane_axis_1 = Z;
		gm.plane_axis_2 = Y;
	} else {
		gm.plane_axis_0 = X;
		gm.plane_axis_1 = Y;
		gm.plane_axis_2 = Z;
	}
	return (TG_OK);
}

uint8_t cm_set_origin_offsets(double x, double y, double z)
{
	cm_set_positions(x, y, z);
	mc_set_position(gm.position[X], gm.position[Y], gm.position[Z]);
	return (TG_OK);
}

uint8_t cm_use_length_units(uint8_t inches_mode)
{
	gm.inches_mode = inches_mode;
	return (TG_OK);
}

uint8_t cm_set_distance_mode(uint8_t absolute_mode)
{
	gm.absolute_mode = absolute_mode;
	return (TG_OK);
}

/* 
 * Free Space Motion (4.3.4)
 *
 * cm_set_traverse_rate() - set seek rate
 * cm_straight_traverse() - G0 linear seek
 */

uint8_t cm_set_traverse_rate(double rate)
{
	rate = _to_millimeters(rate);

	if (rate > gm.max_seek_rate) {
		return (TG_MAX_SEEK_RATE_EXCEEDED);
	} else {
		gm.seek_rate = rate; 		// mm per minute
	}
	return (TG_OK);
}

uint8_t cm_straight_traverse(double x, double y, double z)
{
	// copy parameters into the current state
	gm.next_action = NEXT_ACTION_MOTION;
	gm.motion_mode = MOTION_MODE_STRAIGHT_TRAVERSE;
	cm_set_targets(x, y, z);

	// execute the move
	cm_status = mc_line(gm.target[X], gm.target[Y], gm.target[Z], gm.seek_rate, FALSE);

	// set final position
	cm_set_positions(x, y, z); // ++++ should be able to move this to before mc_line() call
	return (cm_status);
}

/* 
 * Machining Attributes (4.3.5)
 */ 

/*
 * cm_set_feed_rate() - F parameter
 *
 * Sets feed rate; or sets inverse feed rate if it's active.
 * Converts all values to internal format (mm's)
 * Errs out of feed rate exceeds maximum, but doesn't compute maximum for 
 * inverse feed rate as this would require knowing the move length in advance.
 */

uint8_t cm_set_feed_rate(double rate)
{
	if (gm.inverse_feed_rate_mode) {
		gm.inverse_feed_rate = rate; // minutes per motion for this block only
	} else {
		rate = _to_millimeters(rate);
		if (rate > gm.max_feed_rate) {
			return (TG_MAX_FEED_RATE_EXCEEDED);
		} else {
			gm.feed_rate = rate; 	// as mm per minute
		}
	}
	return (TG_OK);
}

/*
 * cm_set_inverse_feed_rate() - G93, G94
 *
 *	TRUE = inverse time feed rate in effect - for this block only
 *	FALSE = units per minute feed rate in effect
 */

inline uint8_t cm_set_inverse_feed_rate_mode(uint8_t mode)
{
	gm.inverse_feed_rate_mode = mode;
	return (TG_OK);
}

/*
 * cm_set_motion_control_mode() - G61, G61.1, G64
 */

uint8_t cm_set_motion_control_mode(uint8_t mode)
{
	return (TG_OK);
}

/* 
 * Machining Functions (4.3.6)
 *
 * (see end of file for arc_feed. It's a long one)
 * cm_dwell() - G4, P parameter (seconds)
 * cm_straight_feed() - G1
 */ 

uint8_t cm_dwell(double seconds)
{
	gm.dwell_time = seconds;
	mc_dwell(seconds);
	return (TG_OK);
}

uint8_t cm_straight_feed(double x, double y, double z)
{
	// copy parameters into the current state
	gm.next_action = NEXT_ACTION_MOTION;
	gm.motion_mode = MOTION_MODE_STRAIGHT_FEED;
	cm_set_targets(x, y, z);

	// execute the move
	cm_status = mc_line(gm.target[X], gm.target[Y], gm.target[Z],  
					  ((gm.inverse_feed_rate_mode) ? gm.inverse_feed_rate : gm.feed_rate),
						gm.inverse_feed_rate_mode);

	// set final position
	/* As far as the gcode engine is concerned the position is now the target.
	 * In reality, motion_control / steppers will still be processing the
	 * action and the real tool position is still close to the starting point.
	 * The endpoint position is not moved if there has been an error.
	 */
	cm_set_positions(x, y, z); // ++++ should be able to move this to before mc_line() call
	return (cm_status);
}

/* 
 * Spindle Functions (4.3.7)
 *
 * cm_set_spindle_speed() - S parameter
 * cm_start_spindle_clockwise() - M3
 * cm_start_spindle_counterclockwise() - M4
 * cm_stop_spindle_turning() - M5
 */

uint8_t cm_set_spindle_speed(double speed)
{
//	if (speed > gm.max_spindle speed) {
//		return (TG_MAX_SPINDLE_SPEED_EXCEEDED);
//	}
	gm.spindle_speed = speed;
	return (TG_OK);
}

uint8_t cm_start_spindle_clockwise(void)
{
	return (TG_OK);
}

uint8_t cm_start_spindle_counterclockwise(void)
{
	return (TG_OK);
}

uint8_t cm_stop_spindle_turning(void)
{
	return (TG_OK);
}

/* 
 * Tool Functions (4.3.8)
 *
 * cm_change_tool() - M6 (This might become a complete tool change cycle)
 * cm_select_tool() - T parameter
 */

uint8_t cm_change_tool(uint8_t tool)
{
	gm.tool = tool;
	return (TG_OK);
}

uint8_t cm_select_tool(uint8_t tool)
{
	gm.tool = tool;
	return (TG_OK);
}

/* 
 * Miscellaneous Functions (4.3.9)
 *
 * cm_comment() - ignore comments (I do)
 * cm_message() - send message to console
 */

uint8_t cm_comment(char *comment)
{
	return (TG_OK);		// no operation
}

uint8_t cm_message(char *message)
{
	printf_P(PSTR("%s\n"), message);
	return (TG_OK);
}

/*
 * Program Functions (4.3.10)
 *
 * This group implements stop, start and end. 
 * It is extended beyond the NIST spec to handle various situations.
 *
 *	cm_program_stop()			(M0, M60)
 *	cm_optional_program_stop()	(M1)
 *	cm_program_end()			(M2, M30)
 *	cm_async_stop()				(no code)
 *	cm_async_start()			(no code)
 *	cm_async_end()				(no code)
 *	cm_stop()					(no code, not implemented)
 *
 * cm_program_stop and cm_optional_program_stop are synchronous Gcode 
 * commands that are received through the interpreter. They cause all motion
 * to stop at the end of the current command, including spindle motion. 
 * Note that the stop occurs at the end of the immediately preceding command
 * (i.e. the stop is queued behind the last command).
 *
 * cm_program_end is a stop that also resets the machine to initial state
 *
 * The three asynchronous commands are not specified in RS724. These commands
 * "jump the queue" and are effective immediately. Async_stop and async_start 
 * can be used in sequence to stop motion in the middle of a move then resume.
 * These are meant to be linked to the keyboard "signals" as so:
 *
 *		<ctrl> c	end immediately (ETX, KILL)
 *		<ctrl> x	end immediately (TERM)
 *		<ctrl> s	stop motion immediately (XOFF)
 *		<ctrl> q	restart motion from  async or queued stop (XON)
 *
 * cm_stop() is a cycle specified by RS274 where the machine pauses for some
 * unspecified length of time then resumes. This is not implemented until 
 * someone cen tell me who uses this and for what, and how it's invoked 
 * - given that there is no coresponding gcode for it.
 */

uint8_t cm_program_stop()					// M0, M60
{
	mc_queued_stop();
	return (TG_OK);
}

uint8_t cm_optional_program_stop()			// M1
{
	mc_queued_stop();
	return (TG_OK);
}

uint8_t cm_program_end()					// M2, M30
{
	mc_queued_end();
	return (TG_OK);
}

uint8_t cm_async_stop()
{
	mc_async_stop();
	return (TG_OK);
}

uint8_t cm_async_start()
{
	mc_async_start();
	return (TG_OK);
}

uint8_t cm_async_end()
{
	mc_async_end();
	return (TG_OK);
}


/*--- CANONICAL MACHINING CYCLES ---*/

uint8_t cm_stop()							// not implemented
{
	return (TG_OK);
}

/* 
 * cm_return_to_home() - G28
 */

uint8_t cm_return_to_home()
{
	return (TG_OK);
}


/***********************************************************************
 *
 * cm_arc_feed() - G2, G3
 * _gc_compute_radius_arc() - compute arc center (offset) from radius.
 * _gc_compute_center_arc() - compute arc from I and J (arc center point)
 *
 * Note: this is mostly original grbl code with little modification other
 * 		 than calling differently and refactoring into smaller routines.
 */

uint8_t cm_arc_feed(double x, double y, double z, // XYZ of the endpoint
					double i, double j, double k, // offsets
					double radius, 				  // non-zero sets radius mode
					uint8_t motion_mode)		  // defined motion mode
{
	// copy parameters into the current state
	gm.next_action = NEXT_ACTION_MOTION;
	gm.motion_mode = motion_mode;
	cm_set_targets(x, y, z);
	cm_set_offsets(i, j, k);
	cm_set_radius(radius);
	cm_status = TG_OK;

	// execute the move
	if (radius) {
		if ((_gc_compute_radius_arc() != TG_OK)) {
			return (cm_status);						// error return
		}
	}
	cm_status = _gc_compute_center_arc();

	// set final position
	if ((cm_status == TG_OK) || (cm_status == TG_EAGAIN)) {
		cm_set_positions(gm.target[X], gm.target[Y], gm.target[Z]);
	}
	return (cm_status);
}

/* _gc_compute_radius_arc() - compute arc center (offset) from radius. */

int _gc_compute_radius_arc()
{
	double x;
	double y;
	double h_x2_div_d;

/*  We need to calculate the center of the circle that has the designated 
	radius and passes through both the current position and the target position
		  
	This method calculates the following set of equations where:
	`  [x,y] is the vector from current to target position, 
		d == magnitude of that vector, 
		h == hypotenuse of the triangle formed by the radius of the circle, 
			 the distance to the center of the travel vector. 
		  
	A vector perpendicular to the travel vector [-y,x] is scaled to the length
	of h [-y/d*h, x/d*h] and added to the center of the travel vector [x/2,y/2]
	to form the new point [i,j] at [x/2-y/d*h, y/2+x/d*h] which will be the 
	center of our arc.
          
       d^2 == x^2 + y^2
       h^2 == r^2 - (d/2)^2
       i == x/2 - y/d*h
       j == y/2 + x/d*h
                                                          O <- [i,j]
                                            -  |
                                  r      -     |
                                      -        |
                                   -           | h
                                -              |
                  [0,0] ->  C -----------------+--------------- T  <- [x,y]
                            | <------ d/2 ---->|
                    
       C - Current position
       T - Target position
       O - center of circle that pass through both C and T
       d - distance from C to T
       r - designated radius
       h - distance from center of CT to O
          
	Expanding the equations:

      	d -> sqrt(x^2 + y^2)
        h -> sqrt(4 * r^2 - x^2 - y^2)/2
        i -> (x - (y * sqrt(4 * r^2 - x^2 - y^2)) / sqrt(x^2 + y^2)) / 2 
        j -> (y + (x * sqrt(4 * r^2 - x^2 - y^2)) / sqrt(x^2 + y^2)) / 2
         
	Which can be written:
          
        i -> (x - (y * sqrt(4 * r^2 - x^2 - y^2))/sqrt(x^2 + y^2))/2
        j -> (y + (x * sqrt(4 * r^2 - x^2 - y^2))/sqrt(x^2 + y^2))/2
          
	Which we for size and speed reasons optimize to:

       	h_x2_div_d = sqrt(4 * r^2 - x^2 - y^2)/sqrt(x^2 + y^2)
      	i = (x - (y * h_x2_div_d))/2
      	j = (y + (x * h_x2_div_d))/2  
	*/
        
	// Calculate the change in position along each selected axis
	x = gm.target[gm.plane_axis_0]-gm.position[gm.plane_axis_0];
	y = gm.target[gm.plane_axis_1]-gm.position[gm.plane_axis_1];

	clear_vector(&gm.offset);
	// == -(h * 2 / d)
	h_x2_div_d = -sqrt(4 * gm.radius*gm.radius - ((x*x) - (y*y))) / hypot(x,y);

	// If r is smaller than d the arc is now traversing the complex plane beyond
	// the reach of any real CNC, and thus - for practical reasons - we will 
	// terminate promptly (well spoken Simen!)
	if(isnan(h_x2_div_d)) { 
		cm_status = TG_FLOATING_POINT_ERROR; 
		return(cm_status); 
	}

	// Invert the sign of h_x2_div_d if circle is counter clockwise 
	// (see sketch below)
	if (gm.motion_mode == MOTION_MODE_CCW_ARC) {
		h_x2_div_d = -h_x2_div_d;
	}

	/*	The counter clockwise circle lies to the left of the target direction. 
		When offset is positive, the left hand circle will be generated - 
		when it is negative the right hand circle is generated.

    
                                     T  <-- Target position
    
                                     ^ 
        Clockwise circles with       |     Clockwise circles with
		this center will have        |     this center will have
        > 180 deg of angular travel  |     < 180 deg of angular travel, 
                          \          |      which is a good thing!
                           \         |         /
    center of arc when  ->  x <----- | -----> x <- center of arc when 
    h_x2_div_d is positive           |             h_x2_div_d is negative
                                     |
    
                                     C  <-- Current position
	*/                

	// Negative R is g-code-alese for "I want a circle with more than 180 degrees
	// of travel" (go figure!), even though it is advised against ever generating
	// such circles in a single line of g-code. By inverting the sign of 
	// h_x2_div_d the center of the circles is placed on the opposite side of 
	// the line of travel and thus we get the unadvisably long arcs as prescribed.
	if (gm.radius < 0) { 
		h_x2_div_d = -h_x2_div_d; 
	}        
        
	// Complete the operation by calculating the actual center of the arc
	gm.offset[gm.plane_axis_0] = (x-(y*h_x2_div_d))/2;
	gm.offset[gm.plane_axis_1] = (y+(x*h_x2_div_d))/2;
	return (cm_status);
} 
    
/*
 * _gc_compute_center_arc() - compute arc from I and J (arc center point)
 */

int _gc_compute_center_arc()
{
	double theta_start;
	double theta_end;
	double angular_travel;
	double radius_tmp;
	double depth;

    /*	This segment sets up an clockwise or counterclockwise arc from the current
		position to the target position around the center designated by the offset
		vector. All theta-values measured in radians of deviance from the positive 
		y-axis. 

                        | <- theta == 0
                      * * *
                    *       *
                  *           *
                  *     O ----T   <- theta_end (e.g. 90 degrees: theta_end == PI/2)
                  *   /
                    C   <- theta_start (e.g. -145 degrees: theta_start == -PI*(3/4))
 	*/

	// calculate the theta (angle) of the current point
	theta_start = _theta(-gm.offset[gm.plane_axis_0], -gm.offset[gm.plane_axis_1]);
	if(isnan(theta_start)) { 
		cm_status = TG_ARC_SPECIFICATION_ERROR;
		return(cm_status); 
	}

	// calculate the theta (angle) of the target point
	theta_end = _theta(gm.target[gm.plane_axis_0] 
					- gm.offset[gm.plane_axis_0] 
					- gm.position[gm.plane_axis_0], 
 					  gm.target[gm.plane_axis_1] 
					- gm.offset[gm.plane_axis_1] 
					- gm.position[gm.plane_axis_1]);

	if(isnan(theta_end)) { 
		cm_status = TG_ARC_SPECIFICATION_ERROR; 
		return(cm_status);
	}

	// ensure that the difference is positive so that we have clockwise travel
	if (theta_end < theta_start) {
		theta_end += 2*M_PI;
	}
	angular_travel = theta_end - theta_start;

	// Invert angular motion if the g-code wanted a counterclockwise arc
	if (gm.motion_mode == MOTION_MODE_CCW_ARC) {
		angular_travel = angular_travel - 2*M_PI;
	}

	// Find the radius
	radius_tmp = hypot(gm.offset[gm.plane_axis_0], gm.offset[gm.plane_axis_1]);

	// Calculate the motion along the depth axis of the helix
	depth = gm.target[gm.plane_axis_2] - gm.position[gm.plane_axis_2];

	// Trace the arc
	cm_status = mc_arc(theta_start, angular_travel, radius_tmp, depth, 
					   gm.plane_axis_0, gm.plane_axis_1, gm.plane_axis_2, 
        	   		  (gm.inverse_feed_rate_mode) ? gm.inverse_feed_rate : gm.feed_rate, 
					   gm.inverse_feed_rate_mode);

    // Finish off with a line to make sure we arrive exactly where we think we are
	//--> For this to work correctly it must be delivered ONLY after the arc generator 
	// has completed the arc. So the endpoint should be passed to the generator and
	// executed there.
//	cm_status = mc_line(gp.target[X_AXIS], gp.target[Y_AXIS], gp.target[Z_AXIS], 
//					   (gp.inverse_feed_rate_mode) ? gp.inverse_feed_rate : 
//						gp.feed_rate, gp.inverse_feed_rate_mode);
	return (cm_status);
}