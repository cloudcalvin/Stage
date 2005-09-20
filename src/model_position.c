///////////////////////////////////////////////////////////////////////////
//
// File: model_laser.c
// Author: Richard Vaughan
// Date: 10 June 2004
//
// CVS info:
//  $Source: /home/tcollett/stagecvs/playerstage-cvs/code/stage/src/model_position.c,v $
//  $Author: gerkey $
//  $Revision: 1.49 $
//
///////////////////////////////////////////////////////////////////////////


#include <sys/time.h>
#include <math.h>
#include <stdlib.h>

//#define DEBUG

#include "stage_internal.h"
#include "gui.h"

// move into header
void stg_model_position_odom_reset( stg_model_t* mod );
void stg_model_position_get_odom( stg_model_t* mod, stg_pose_t* odom );

//extern stg_rtk_fig_t* fig_debug_rays;

/** 
@ingroup model
@defgroup model_position Position model 

The position model simulates a
mobile robot base. It can drive in one of two modes; either
<i>differential</i>, i.e. able to control its speed and turn rate by
driving left and roght wheels like a Pioneer robot, or
<i>omnidirectional</i>, i.e. able to control each of its three axes
independently.

<h2>Worldfile properties</h2>

@par Summary and default values

@verbatim
position
(
  drive "diff"

  localization "gps"

  # initial position estimate
  localization_origin [ <defaults to model's start pose> ]
 
  # odometry error model parameters, 
  # only used if localization is set to "odom"
  odom_error [0.03 0.03 0.05]
)
@endverbatim

@par Note
Since Stage-1.6.5 the odom property has been removed. Stage will generate a warning if odom is defined in your worldfile. See localization_origin instead.

@par Details
- drive "diff" or "omni"
  - select differential-steer mode (like a Pioneer) or omnidirectional mode.
- localization "gps" or "odom"
  - if "gps" the position model reports its position with perfect accuracy. If "odom", a simple odometry model is used and position data drifts from the ground truth over time. The odometry model is parameterized by the odom_error property.
- localization_origin [x y theta]
  - set the origin of the localization coordinate system. By default, this is copied from the model's initial pose, so the robot reports its position relative to the place it started out. Tip: If localization_origin is set to [0 0 0] and localization is "gps", the model will return its true global position. This is unrealistic, but useful if you want to abstract away the details of localization. Be prepared to justify the use of this mode in your research! 
- odom_error [x y theta]
  - parameters for the odometry error model used when specifying localization "odom". Each value is the maximum proportion of error in intergrating x, y, and theta velocities to compute odometric position estimate. For each axis, if the the value specified here is E, the actual proportion is chosen at startup at random in the range -E/2 to +E/2. Note that due to rounding errors, setting these values to zero does NOT give you perfect localization - for that you need to choose localization "gps".
*/

/** 
@ingroup stg_model_position
@ingroup stg_model_props
@defgroup stg_model_position_props Position Properties

- "position_drive" #stg_position_drive_mode_t
- "position_data" stg_position_data_t
- "position_cmd" stg_position_cmd_t
*/


const double STG_POSITION_WATTS_KGMS = 5.0; // cost per kg per meter per second
const double STG_POSITION_WATTS = 10.0; // base cost of position device

// simple odometry error model parameters. the error is selected at
// random in the interval -MAX/2 to +MAX/2 at startup
const double STG_POSITION_INTEGRATION_ERROR_MAX_X = 0.03;
const double STG_POSITION_INTEGRATION_ERROR_MAX_Y = 0.03;
const double STG_POSITION_INTEGRATION_ERROR_MAX_A = 0.05;

int position_startup( stg_model_t* mod );
int position_shutdown( stg_model_t* mod );
int position_update( stg_model_t* mod );
void position_load( stg_model_t* mod );
int position_render_data( stg_model_t* mod, char* name, void* data, size_t len, void* userp );
int position_unrender_data( stg_model_t* mod, char* name, void* data, size_t len, void* userp );

int position_init( stg_model_t* mod )
{
  PRINT_DEBUG( "created position model" );
  
  static int first_time = 1;

  if( first_time )
    {
      first_time = 0;
      // seed the RNG on startup
      srand48( time(NULL) );
    }

  // no power consumed until we're subscribed
  //mod->watts = 0.0; 

  // override the default methods
  mod->f_startup = position_startup;
  mod->f_shutdown = position_shutdown;
  mod->f_update = position_update;
  mod->f_load = position_load;

  // sensible position defaults

  stg_velocity_t vel;
  memset( &vel, 0, sizeof(vel));
  stg_model_set_property( mod, "velocity", &vel, sizeof(vel));
  
  stg_blob_return_t blb = 1;
  stg_model_set_property( mod, "blob_return", &blb, sizeof(blb));
  
  stg_position_drive_mode_t drive = STG_POSITION_DRIVE_DEFAULT;  
  stg_model_set_property( mod, "position_drive", &drive, sizeof(drive) );

  stg_position_stall_t stall = 0;
  stg_model_set_property( mod, "position_stall", &stall, sizeof(stall));
  
  stg_position_cmd_t cmd;
  memset( &cmd, 0, sizeof(cmd));
  cmd.mode = STG_POSITION_CONTROL_DEFAULT;
  stg_model_set_property( mod, "position_cmd", &cmd, sizeof(cmd));
  
  stg_position_data_t data;
  memset( &data, 0, sizeof(data));
  
  data.integration_error.x =  
    drand48() * STG_POSITION_INTEGRATION_ERROR_MAX_X - 
    STG_POSITION_INTEGRATION_ERROR_MAX_X/2.0;
  
  data.integration_error.y =  
    drand48() * STG_POSITION_INTEGRATION_ERROR_MAX_Y - 
    STG_POSITION_INTEGRATION_ERROR_MAX_Y/2.0;

  data.integration_error.a =  
    drand48() * STG_POSITION_INTEGRATION_ERROR_MAX_A - 
    STG_POSITION_INTEGRATION_ERROR_MAX_A/2.0;

  data.localization = STG_POSITION_LOCALIZATION_DEFAULT;

  stg_model_set_property( mod, "position_data", &data, sizeof(data));
  
  stg_model_add_property_toggles( mod, "position_data",  
 				  position_render_data, // called when toggled on
 				  NULL, 
 				  position_unrender_data, // called when toggled off 
 				  NULL,  
 				  "position data", 
				  FALSE ); 
    
  
  return 0;
}

void position_load( stg_model_t* mod )
{
  char* keyword = NULL;

  // load steering mode
  if( wf_property_exists( mod->id, "drive" ) )
    {
      stg_position_drive_mode_t* now = 
	stg_model_get_property_fixed( mod, "position_drive", 
				     sizeof(stg_position_drive_mode_t));
      
      stg_position_drive_mode_t drive =
	now ? *now : STG_POSITION_DRIVE_DIFFERENTIAL;
      
      const char* mode_str =  
	wf_read_string( mod->id, "drive", NULL );
      
      if( mode_str )
	{
	  if( strcmp( mode_str, "diff" ) == 0 )
	    drive = STG_POSITION_DRIVE_DIFFERENTIAL;
	  else if( strcmp( mode_str, "omni" ) == 0 )
	    drive = STG_POSITION_DRIVE_OMNI;
	  else
	    {
	      PRINT_ERR1( "invalid position drive mode specified: \"%s\" - should be one of: \"diff\", \"omni\". Using \"diff\" as default.", mode_str );	      
	    }	 
	  stg_model_set_property( mod, "position_drive", &drive, sizeof(drive)); 
	}
    }      
  
  stg_position_data_t* data = 
    stg_model_get_property_fixed( mod, "position_data", sizeof(stg_position_data_t));
  assert( data );
  
  // load odometry if specified
  if( wf_property_exists( mod->id, "odom" ) )
    {
      PRINT_WARN1( "the odom property is specified for model \"%s\","
		   " but this property is no longer available."
		   " Use localization_origin instead. See the position"
		   " entry in the manual or src/model_position.c for details.", 
		   mod->token );
    }

  // set the starting pose as my initial odom position. This could be
  // overwritten below if the localization_origin property is
  // specified
  stg_model_get_global_pose( mod, &data->origin );

  keyword = "localization_origin"; 
  if( wf_property_exists( mod->id, keyword ) )
    {  
      data->origin.x = wf_read_tuple_length(mod->id, keyword, 0, data->pose.x );
      data->origin.y = wf_read_tuple_length(mod->id, keyword, 1, data->pose.y );
      data->origin.a = wf_read_tuple_angle(mod->id, keyword, 2, data->pose.a );

      // compute our localization pose based on the origin and true pose
      stg_pose_t gpose;
      stg_model_get_global_pose( mod, &gpose );
      
      data->pose.a = NORMALIZE( gpose.a - data->origin.a );
      double cosa = cos(data->pose.a);
      double sina = sin(data->pose.a);
      double dx = gpose.x - data->origin.x;
      double dy = gpose.y - data->origin.y; 
      data->pose.x = dx * cosa + dy * sina; 
      data->pose.y = dy * cosa - dx * sina;

      // zero position error: assume we know exactly where we are on startup
      memset( &data->pose_error, 0, sizeof(data->pose_error));      
    }

  // odometry model parameters
  if( wf_property_exists( mod->id, "odom_error" ) )
    {
      data->integration_error.x = 
	wf_read_tuple_length(mod->id, "odom_error", 0, data->integration_error.x );
      data->integration_error.y = 
	wf_read_tuple_length(mod->id, "odom_error", 1, data->integration_error.y );
      data->integration_error.a 
	= wf_read_tuple_angle(mod->id, "odom_error", 2, data->integration_error.a );
    }

  // choose a localization model
  if( wf_property_exists( mod->id, "localization" ) )
    {
      const char* loc_str =  
	wf_read_string( mod->id, "localization", NULL );
   
      if( loc_str )
	{
	  if( strcmp( loc_str, "gps" ) == 0 )
	    data->localization = STG_POSITION_LOCALIZATION_GPS;
	  else if( strcmp( loc_str, "odom" ) == 0 )
	    data->localization = STG_POSITION_LOCALIZATION_ODOM;
	  else
	    PRINT_ERR2( "unrecognized localization mode \"%s\" for model \"%s\"."
			" Valid choices are \"gps\" and \"odom\".", 
			loc_str, mod->token );
	}
      else
	PRINT_ERR1( "no localization mode string specified for model \"%s\"", 
		    mod->token );
    }

  // we've probably poked the localization data, so we must refresh it
  stg_model_property_refresh( mod, "position_data" );


}


int position_update( stg_model_t* mod )
{ 
      
  PRINT_DEBUG1( "[%lu] position update", mod->world->sim_time );
  
  stg_position_data_t* data = 
    stg_model_get_property_fixed( mod, "position_data", 
				  sizeof(stg_position_data_t));
  assert(data);
  
  stg_velocity_t* vel = 
    stg_model_get_property_fixed( mod, "velocity", 
				  sizeof(stg_velocity_t));
  assert(vel);
  
  // stop by default
  memset( vel, 0, sizeof(stg_velocity_t) );
  
  if( mod->subs )   // no driving if noone is subscribed
    {            
      stg_position_cmd_t *cmd = 
	stg_model_get_property_fixed( mod, "position_cmd", sizeof(stg_position_cmd_t));      
      assert(cmd);
      
      stg_position_drive_mode_t *drive = 
	stg_model_get_property_fixed( mod, "position_drive", sizeof(stg_position_drive_mode_t));
      assert(drive);
      
      //printf( "cmd mode %d x %.2f y %.2f a %.2f\n",
      //      cmd.mode, cmd.x, cmd.y, cmd.a );

      
      switch( cmd->mode )
	{
	case STG_POSITION_CONTROL_VELOCITY :
	  {
	    PRINT_DEBUG( "velocity control mode" );
	    PRINT_DEBUG4( "model %s command(%.2f %.2f %.2f)",
			  mod->token, 
			  cmd->x, 
			  cmd->y, 
			  cmd->a );
	    
	    switch( *drive )
	      {
	      case STG_POSITION_DRIVE_DIFFERENTIAL:
		// differential-steering model, like a Pioneer
		vel->x = cmd->x;
		vel->y = 0;
		vel->a = cmd->a;
		break;
		
	      case STG_POSITION_DRIVE_OMNI:
		// direct steering model, like an omnidirectional robot
		vel->x = cmd->x;
		vel->y = cmd->y;
		vel->a = cmd->a;
		break;
		
	      default:
		PRINT_ERR1( "unknown steering mode %d", *drive );
	      }
	  } break;
	  
	case STG_POSITION_CONTROL_POSITION:
	  {
	    PRINT_DEBUG( "position control mode" );
	    
	    double x_error = cmd->x - data->pose.x;
	    double y_error = cmd->y - data->pose.y;
	    double a_error = NORMALIZE( cmd->a - data->pose.a );
	    
	    PRINT_DEBUG3( "errors: %.2f %.2f %.2f\n", x_error, y_error, a_error );
	    
	    // speed limits for controllers
	    // TODO - have these configurable
	    double max_speed_x = 0.4;
	    double max_speed_y = 0.4;
	    double max_speed_a = 1.0;	      
	    
	    switch( *drive )
	      {
	      case STG_POSITION_DRIVE_OMNI:
		{
		  // this is easy - we just reduce the errors in each axis
		  // independently with a proportional controller, speed
		  // limited
		  vel->x = MIN( x_error, max_speed_x );
		  vel->y = MIN( y_error, max_speed_y );
		  vel->a = MIN( a_error, max_speed_a );
		}
		break;
		
	      case STG_POSITION_DRIVE_DIFFERENTIAL:
		{
		  // axes can not be controlled independently. We have to
		  // turn towards the desired x,y position, drive there,
		  // then turn to face the desired angle.  this is a
		  // simple controller that works ok. Could easily be
		  // improved if anyone needs it better. Who really does
		  // position control anyhoo?
		  
		  // start out with no velocity
		  stg_velocity_t calc;
		  memset( &calc, 0, sizeof(calc));
		  
		  double close_enough = 0.02; // fudge factor
		  
		  // if we're at the right spot
		  if( fabs(x_error) < close_enough && fabs(y_error) < close_enough )
		    {
		      PRINT_DEBUG( "TURNING ON THE SPOT" );
		      // turn on the spot to minimize the error
		      calc.a = MIN( a_error, max_speed_a );
		      calc.a = MAX( a_error, -max_speed_a );
		    }
		  else
		    {
		      PRINT_DEBUG( "TURNING TO FACE THE GOAL POINT" );
		      // turn to face the goal point
		      double goal_angle = atan2( y_error, x_error );
		      double goal_distance = hypot( y_error, x_error );
		      
		      a_error = NORMALIZE( goal_angle - data->pose.a );
		      calc.a = MIN( a_error, max_speed_a );
		      calc.a = MAX( a_error, -max_speed_a );
		      
		      PRINT_DEBUG2( "steer errors: %.2f %.2f \n", a_error, goal_distance );
		      
		      // if we're pointing about the right direction, move
		      // forward
		      if( fabs(a_error) < M_PI/16 )
			{
			  PRINT_DEBUG( "DRIVING TOWARDS THE GOAL" );
			  calc.x = MIN( goal_distance, max_speed_x );
			}
		    }
		  
		  // now set the underlying velocities using the normal
		  // diff-steer model
		  //vel->x = (calc.x * cos(mod->pose.a) - calc.y * sin(mod->pose.a));
		  //vel->y = (calc.x * sin(mod->pose.a) + calc.y * cos(mod->pose.a));
		  vel->x = calc.x;
		  vel->y = 0;
		  vel->a = calc.a;
		}
		break;
		
	      default:
		PRINT_ERR1( "unknown steering mode %d", (int)drive );
	      }
	  }
	  break;
	  
	default:
	  PRINT_ERR1( "unrecognized position command mode %d", cmd->mode );
	}
      
      // simple model of power consumption
      // mod->watts = STG_POSITION_WATTS + 
      //fabs(vel->x) * STG_POSITION_WATTS_KGMS * mod->mass + 
      //fabs(vel->y) * STG_POSITION_WATTS_KGMS * mod->mass + 
      //fabs(vel->a) * STG_POSITION_WATTS_KGMS * mod->mass; 

      //PRINT_DEBUG4( "model %s velocity (%.2f %.2f %.2f)",
      //	    mod->token, 
      //	    mod->velocity.x, 
      //	    mod->velocity.y,
      //	    mod->velocity.a );
      

      stg_position_stall_t stall = 0;
      stg_model_set_property( mod, "position_stall", &stall, sizeof(stall));


      // we've poked the velocity - muts refresh it so others notice
      // the change
      stg_model_property_refresh( mod, "velocity" );
    }
  
  // now  inherit the normal update - this does the actual moving
  _model_update( mod );
  
  switch( data->localization )
    {
    case STG_POSITION_LOCALIZATION_GPS:
      {
	// compute our localization pose based on the origin and true pose
	stg_pose_t gpose;
	stg_model_get_global_pose( mod, &gpose );
	
	data->pose.a = NORMALIZE( gpose.a - data->origin.a );
	//data->pose.a =0;// NORMALIZE( gpose.a - data->origin.a );
	double cosa = cos(data->origin.a);
	double sina = sin(data->origin.a);
	double dx = gpose.x - data->origin.x;
	double dy = gpose.y - data->origin.y; 
	data->pose.x = dx * cosa + dy * sina; 
	data->pose.y = dy * cosa - dx * sina;
      }
      break;
      
    case STG_POSITION_LOCALIZATION_ODOM:
      {
	// integrate our velocities to get an 'odometry' position estimate.
	double dt = mod->world->sim_interval/1e3;
	
	data->pose.a = NORMALIZE( data->pose.a + (vel->a * dt) * (1.0 +data->integration_error.a) );
	
	double cosa = cos(data->pose.a);
	double sina = sin(data->pose.a);
	double dx = (vel->x * dt) * (1.0 + data->integration_error.x );
	double dy = (vel->y * dt) * (1.0 + data->integration_error.y );
	
	data->pose.x += dx * cosa + dy * sina; 
	data->pose.y -= dy * cosa - dx * sina;
      }
      break;
      
    default:
      PRINT_ERR2( "unknown localization mode %d for model %s\n",
		  data->localization, mod->token );
      break;
    }
  
  // we've probably poked the position data - must refresh 
  stg_model_property_refresh( mod, "position_data" );
 
  return 0; //ok
}

int position_startup( stg_model_t* mod )
{
  PRINT_DEBUG( "position startup" );

  //mod->watts = STG_POSITION_WATTS;

  
  //stg_model_position_odom_reset( mod );

  return 0; // ok
}

int position_shutdown( stg_model_t* mod )
{
  PRINT_DEBUG( "position shutdown" );
  
  // safety features!
  stg_position_cmd_t cmd;
  memset( &cmd, 0, sizeof(cmd) ); 
  stg_model_set_property( mod, "position_cmd", &cmd, sizeof(cmd));
   
  stg_velocity_t vel;
  memset( &vel, 0, sizeof(vel));
  stg_model_set_property( mod, "velocity", &vel, sizeof(vel) );
  
  return 0; // ok
}

int position_unrender_data( stg_model_t* mod, char* name, 
			    void* data, size_t len, void* userp )
{
  stg_model_fig_clear( mod, "position_data_fig" );
  return 1;
}

int position_render_data( stg_model_t* mod, char* name, 
			  void* data, size_t len, void* userp )
{
  stg_rtk_fig_t* fig = stg_model_get_fig( mod, "position_data_fig" );
  
  if( !fig )
    {
      fig = stg_model_fig_create( mod, "position_data_fig", NULL, STG_LAYER_POSITIONDATA );
      //stg_rtk_fig_color_rgb32( fig, 0x9999FF ); // pale blue
      
      stg_color_t* col = stg_model_get_property_fixed( mod, "color", sizeof(stg_color_t));
      assert(col);

      stg_rtk_fig_color_rgb32( fig, *col ); 
    }

  stg_rtk_fig_clear(fig);
	  
  if( mod->subs )
    {  
      stg_position_data_t* odom = (stg_position_data_t*)data;
      
      stg_velocity_t* vel = 
	stg_model_get_property_fixed( mod, "velocity", sizeof(stg_velocity_t));
      
      //stg_rtk_fig_origin( fig,  odom->pose.x, odom->pose.y, odom->.a );
      stg_rtk_fig_origin( fig,  odom->origin.x, odom->origin.y, odom->origin.a );
      
      stg_rtk_fig_rectangle(  fig, 0,0,0, 0.06, 0.06, 0 );
      stg_rtk_fig_line( fig, 0,0, odom->pose.x, 0);
      stg_rtk_fig_line( fig, odom->pose.x, 0, odom->pose.x, odom->pose.y );
      
      char buf[256];
      snprintf( buf, 255, "vel(%.3f,%.3f,%.1f)\npos(%.3f,%.3f,%.1f)", 
		vel->x, vel->y, vel->a,
		odom->pose.x, odom->pose.y, odom->pose.a ); 
      
      stg_rtk_fig_text( fig, odom->pose.x + 0.4, odom->pose.y + 0.2, 0, buf );

      // draw an outline of the position model
      stg_geom_t *geom = 
	stg_model_get_property_fixed( mod, "geom", sizeof(stg_geom_t));
      assert(geom);

      stg_rtk_fig_rectangle(  fig, 
			      odom->pose.x, odom->pose.y, odom->pose.a,
			      0.1, 0.1, 0 );

      stg_rtk_fig_arrow( fig, 
			 odom->pose.x, odom->pose.y, odom->pose.a, 
			 geom->size.x/2.0, geom->size.y/2.0 );

      //stg_pose_t gpose;
      //stg_model_get_global_pose( mod, &gpose );
      //stg_rtk_fig_line( fig, gpose.x, gpose.y, odom->pose.x, odom->pose.y );
      
    }

  return 0;
}
