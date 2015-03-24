/* Copyright (c) 2015, Stefan Isler, islerstefan@bluewin.ch
*
This file is part of dense_reconstruction, a ROS package for...well,

dense_reconstruction is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
dense_reconstruction is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Lesser General Public License for more details.
You should have received a copy of the GNU Lesser General Public License
along with dense_reconstruction. If not, see <http://www.gnu.org/licenses/>.
*/

#include "dense_reconstruction/view_planner.h"
#include "boost/foreach.hpp"
#include "utils/math.h"
#include <fstream>
#include <sstream>


namespace dense_reconstruction
{

ViewPlanner::ViewPlanner( ros::NodeHandle& _n )
  :nh_(_n)
  ,start_(false)
  ,pause_(false)
  ,stop_and_print_(false)
  ,reinit_(false)
  ,abort_loop_(false)
{
  if( !nh_.getParam("/view_planner/data_folder", data_folder_) )
  {
    ROS_WARN("No data folder was found on parameter server. Planning data will be saved to ROS execution directory...");
  }
  
  if( !nh_.getParam("/view_planner/cost_weight", cost_weight_) )
  {
    ROS_WARN("No cost weight was found on parameter server. default '1.0' will be used...");
    cost_weight_ = 1.0;
  }
  
  view_space_retriever_ = nh_.serviceClient<dense_reconstruction::FeasibleViewSpaceRequest>("/dense_reconstruction/robot_interface/feasible_view_space");
  current_view_retriever_ = nh_.serviceClient<dense_reconstruction::ViewRequest>("/dense_reconstruction/robot_interface/current_view");
  data_retriever_ = nh_.serviceClient<dense_reconstruction::RetrieveData>("/dense_reconstruction/robot_interface/retrieve_data");
  cost_retriever_ = nh_.serviceClient<dense_reconstruction::MovementCostCalculation>("/dense_reconstruction/robot_interface/movement_cost");
  view_information_retriever_ = nh_.serviceClient<dense_reconstruction::ViewInformationReturn>("/dense_reconstruction/3d_model/information");
  robot_mover_ = nh_.serviceClient<dense_reconstruction::MoveToOrder>("/dense_reconstruction/robot_interface/move_to");
  
  planning_frame_ = "dr_origin";
  metrics_to_use_.push_back("NrOfUnknownVoxels");
  metrics_to_use_.push_back("AverageUncertainty");
  metrics_to_use_.push_back("AverageEndPointUncertainty");
  metrics_to_use_.push_back("UnknownObjectSideFrontier");
  metrics_to_use_.push_back("UnknownObjectVolumeFrontier");
  metrics_to_use_.push_back("ClassicFrontier");
  metrics_to_use_.push_back("EndNodeOccupancySum");
  metrics_to_use_.push_back("TotalOccupancyCertainty");
  metrics_to_use_.push_back("TotalNrOfOccupieds");
  
  BOOST_FOREACH( auto metric_name, metrics_to_use_ )
  {
    double weight=0;
    if( !nh_.getParam("/view_planner/information_metric/"+metric_name+"/weight", weight) )
    {
      ROS_WARN_STREAM("No weight found on parameter server for "<<metric_name<<" metric ("<<"/view_planner/information_metric/"<<metric_name<<"/weight"<<"). Weight will be set to zero and the metric thus not considered in calculations.");
    }
    information_weights_.push_back(weight);
  }
  
  planning_data_names_.push_back("pos_x");
  planning_data_names_.push_back("pos_y");
  planning_data_names_.push_back("pos_z");
  planning_data_names_.push_back("rot_x");
  planning_data_names_.push_back("rot_y");
  planning_data_names_.push_back("rot_z");
  planning_data_names_.push_back("rot_w");
  planning_data_names_.push_back("return_value");
  planning_data_names_.push_back("winning_margin");
  planning_data_names_.push_back("return_value_mean");
  planning_data_names_.push_back("return_value_stddev");
  planning_data_names_.push_back("cost");
  planning_data_names_.push_back("NrOfUnknownVoxels");
  planning_data_names_.push_back("AverageUncertainty");
  planning_data_names_.push_back("AverageEndPointUncertainty");
  planning_data_names_.push_back("UnknownObjectSideFrontier");
  planning_data_names_.push_back("UnknownObjectVolumeFrontier");
  planning_data_names_.push_back("ClassicFrontier");
  planning_data_names_.push_back("EndNodeOccupancySum");
  planning_data_names_.push_back("TotalOccupancyCertainty");
  planning_data_names_.push_back("TotalNrOfOccupieds");
  
  command_ = nh_.subscribe( "/dense_reconstruction/view_planner/command", 1, &ViewPlanner::commandCallback, this );
}

void ViewPlanner::run()
{
  while( !start_ ) // wait for start signal
  {
    waitAndSpin();
  }
  // get view space from robot_interface
  while( !getViewSpace() )
  {
    ROS_INFO("View space service not available yet. Waiting...");
    waitAndSpin(2);
  }
  
  // get current view
  while( !getCurrentView(current_view_) )
  {
    ROS_INFO("Attempting to retrieve start view. Waiting...");
    waitAndSpin(2);
  }
  
  // gather initial data
  retrieveDataAndWait();
  
  
  // enter loop
  do
  {
    pauseIfRequested();
    
    // possibly build subspace of complete space
    std::vector<unsigned int> views_to_consider;
    determineAvailableViewSpace( views_to_consider );
    
    // get movement costs
    ROS_INFO("Retrieve movement costs...");
    std::vector<double> cost(views_to_consider.size());
    for( unsigned int i=0; i<cost.size(); i++ )
    {
      RobotPlanningInterface::MovementCost cost_description;
      View target = view_space_.getView( views_to_consider[i] );
      movementCost( cost_description, current_view_, target );
      
      if( cost_description.exception!=RobotPlanningInterface::MovementCost::NONE )
      {
	view_space_.setBad( views_to_consider[i] ); // don't consider that view
	cost[i] = -1; // no negative costs exist, that's to mark invalids
      }
      else
      {
	cost[i] = cost_description.cost;
      }
    }
    pauseIfRequested();
    
    // get expected informations for each
    ROS_INFO("Retrieve information gain...");
    std::vector< std::vector<double> > information(views_to_consider.size());
    for( unsigned int i=0; i<information.size(); i++ )
    {
      if( cost[i]==-1 )
	continue;
      
      View target_view = view_space_.getView( views_to_consider[i] );
      
      movements::PoseVector target_positions;
      target_positions.push_back( target_view.pose() );
      
      getViewInformation( information[i], target_positions );
    }
    
    pauseIfRequested();
    
    ROS_INFO("Calculating next best view...");
    // calculate return for each
    std::vector<double> view_returns(views_to_consider.size());
    for( unsigned int i=0; i<view_returns.size(); ++i )
    {
      if( cost[i]==-1 )
	continue;
      
      view_returns[i] = calculateReturn( cost[i], information[i] );
    }
    
    // calculate NBV
    unsigned int nbv_index;
    double highest_return = 0;
    double second_highest = 0;
    for( unsigned int i=0; i<views_to_consider.size(); ++i )
    {
      if( cost[i]==-1 )
	continue;
      
      if( view_returns[i]>highest_return )
      {
	nbv_index = i;
	second_highest = highest_return;
	highest_return = view_returns[i];
      }
    }
    
    // data storage...
    ReturnValueInformation return_info;
    return_info.return_value = highest_return;
    return_info.winning_margin = highest_return - second_highest;
    st_is::StdError return_val_errors(view_returns);
    return_info.return_value_mean = return_val_errors.mean;
    return_info.return_value_stddev = std::sqrt(return_val_errors.variance);
    
    saveNBVData(nbv_index, return_info, cost[nbv_index], information[nbv_index] );
    
    // check if termination criteria is fulfilled
    if( !terminationCriteriaFulfilled(highest_return, cost[nbv_index], information[nbv_index]) )
    {
      // move to this view
      View nbv = view_space_.getView(nbv_index);
      moveToAndWait(nbv);
      
      // retrieve new information
      retrieveDataAndWait();
    }
    else
    {
      // reconstruction is done, end iteration
      ROS_INFO("The termination critera was fulfilled and the reconstruction is thus considered to have succeeded. The view planner will shut down.");
      break;
    }
    
    ros::spinOnce();
  }while(!stop_and_print_);
  
  ROS_INFO("Saving data to file.");
  saveDataToFile();
}

void ViewPlanner::waitAndSpin(double _sec)
{
  ros::Duration(0.5).sleep();
  ros::spinOnce();
}

void ViewPlanner::pauseIfRequested()
{
  if( pause_ )
  {
    ROS_INFO("Paused.");
    do
    {
      ros::Duration(1.0).sleep();
      ros::spinOnce();
    }while( pause_ );
  }
}

void ViewPlanner::determineAvailableViewSpace( std::vector<unsigned int>& _output )
{
  view_space_.getGoodViewSpace(_output);
}

double ViewPlanner::calculateReturn( double _cost, std::vector<double>& _informations )
{
  double view_return = -1*cost_weight_*_cost;
  
  if( _informations.size()>information_weights_.size() )
  {
    ROS_ERROR_STREAM("ViewPlanner::calculateReturn::Not enough information weights available ("<<information_weights_.size()<<") for the number of information values given ("<<_informations.size()<<") Information is not considered for return value.");
    return view_return;
  }
  
  for( unsigned int i=0; i<_informations.size(); ++i )
  {
    view_return += information_weights_[i]*_informations[i];
  }
  
  return view_return;
}

bool ViewPlanner::terminationCriteriaFulfilled( double _return_value, double _cost, std::vector<double>& _information_gain )
{
  // TODO: need to test which values/metrics saturate and then pick one of these and test it for saturation
  return false;
}

void ViewPlanner::saveNBVData( unsigned int _nbv_index, ReturnValueInformation& _return_value_information, double _cost, std::vector<double>& _information_gain, std::vector<std::string>* _additional_field_names, std::vector<double>* _additional_field_values )
{
  std::vector<double> nbv_data;
  View nbv = view_space_.getView(_nbv_index);
  
  nbv_data.push_back( nbv.pose().position.x() );
  nbv_data.push_back( nbv.pose().position.y() );
  nbv_data.push_back( nbv.pose().position.z() );
  nbv_data.push_back( nbv.pose().orientation.x() );
  nbv_data.push_back( nbv.pose().orientation.y() );
  nbv_data.push_back( nbv.pose().orientation.z() );
  nbv_data.push_back( nbv.pose().orientation.w() );
  nbv_data.push_back( _return_value_information.return_value );
  nbv_data.push_back( _return_value_information.winning_margin );
  nbv_data.push_back( _return_value_information.return_value_mean );
  nbv_data.push_back( _return_value_information.return_value_stddev );
  nbv_data.push_back( _cost );
  BOOST_FOREACH( auto information, _information_gain )
  {
    nbv_data.push_back( information );
  }
  
  if( _additional_field_names!=nullptr && _additional_field_values!=nullptr )
  {
    if( _additional_field_names->size()==_additional_field_values->size() )
    {
      for( unsigned int i=0; i<_additional_field_names->size(); ++i )
      {
	unsigned int index = getIndexForAdditionalField((*_additional_field_names)[i]);
	if( nbv_data.size()<=index )
	{
	  nbv_data.resize( index+1 );
	}
	nbv_data[index] = (*_additional_field_values)[i];
      }
    }
  }
  
  planning_data_.push_back(nbv_data);
}

void ViewPlanner::saveDataToFile()
{
  std::stringstream filename;
  ros::Time now = ros::Time::now();
  filename << data_folder_<<"planning_data"<<ros::Time::now()<<".data";
  std::string file_name = filename.str();
  
  std::ofstream out(file_name, std::ofstream::trunc);
  
  // first line with names
  out<<planning_data_names_[0];
  for( unsigned int i=1; i<planning_data_names_.size(); ++i )
  {
    out<<" "<<planning_data_names_[i];
  }
  // then all values
  for( unsigned int i=0; i<planning_data_.size(); ++i )
  {
    out<<"\n";
    out<<planning_data_[i][0];
    for( unsigned int j=1; j<planning_data_[i].size(); ++j )
    {
      out<<" "<<planning_data_[i][j];
    }
  }
  out.close();
}

void ViewPlanner::retrieveDataAndWait( double _sec )
{
  RobotPlanningInterface::ReceiveInfo receive_info;
  bool receive_service_succeeded = false;
  do
  {
    receive_service_succeeded = retrieveData(receive_info);
    if( !receive_service_succeeded || receive_info!=RobotPlanningInterface::RECEIVED )
    {
      ROS_INFO("retrieveDataAndWait: data retrieval service did not succeed. Trying again in a few...");
      ros::Duration(_sec).sleep();
      ros::spinOnce();
    }
  }while( (!receive_service_succeeded || receive_info!=RobotPlanningInterface::RECEIVED) && !abort_loop_ );
  
  if( abort_loop_ )
  {
    ROS_INFO("retrieveDataAndWait received loop abortion request and stops trying to get the data retrieval service to succeed. The service might have failed.");
    abort_loop_ = false;
  }
  else
  {
    ROS_INFO("Data retrieval service reported successful data retrieval.");
  }
}

void ViewPlanner::moveToAndWait( View& _target_view, double _sec )
{
  bool successfully_moved;
  bool service_succeeded = false;
  do
  {
    service_succeeded = moveTo( successfully_moved, _target_view );
    if( !service_succeeded || !successfully_moved )
    {
      ROS_INFO("moveToAndWait: Either the service failed or the robot movement. Trying again in a few...");
      ros::Duration(_sec).sleep();
      ros::spinOnce();
    }
  }while( (!service_succeeded || !successfully_moved) && !abort_loop_ );
  
  if( abort_loop_ )
  {
    ROS_INFO("moveToAndWait received loop abortion request and stops trying to get the robot movement service to succeed. The service might have failed.");
    abort_loop_ = false;
  }
  else
  {
    ROS_INFO("Robot movement service reported successful movement.");
  }
}

unsigned int ViewPlanner::getIndexForAdditionalField( std::string _name )
{
  for( unsigned int i=0; i<planning_data_names_.size(); ++i )
  {
    if( planning_data_names_[i]==_name )
      return i;
  }
  
  planning_data_names_.push_back(_name);
  return planning_data_names_.size()-1;
}

bool ViewPlanner::getViewSpace()
{
  FeasibleViewSpaceRequest request;
  
  bool response = view_space_retriever_.call(request);
  
  if( response )
  {
    view_space_.fromMsg( request.response.view_space );
  }
  
  return response;
}

bool ViewPlanner::getCurrentView( View& _output)
{
  ViewRequest request;
  
  bool response = current_view_retriever_.call(request);
  
  if( response )
  {
    View out( request.response.view );
    _output = out;
  }
  
  return response;
}

bool ViewPlanner::retrieveData( RobotPlanningInterface::ReceiveInfo& _output )
{
  RetrieveData request;
  
  bool response = data_retriever_.call(request);
  
  if( response )
  {
    _output = RobotPlanningInterface::ReceiveInfo( request.response.receive_info );
  }
  
  return response;
}

bool ViewPlanner::movementCost( RobotPlanningInterface::MovementCost& _output, View& _start_view, View& _target_view )
{
  MovementCostCalculation request;
  request.request.start_view = _start_view.toMsg();
  request.request.target_view = _target_view.toMsg();
  request.request.additional_information = true;
  
  bool response = cost_retriever_.call(request);
  
  if( response )
  {
    _output.fromMsg( request.response.movement_cost );
  }
  
  return response;
}

bool ViewPlanner::moveTo( bool& _output, View& _target_view )
{
  MoveToOrder request;
  request.request.target_view = _target_view.toMsg();
  
  bool response = data_retriever_.call(request);
  
  if( response )
  {
    _output = request.response.success;
  }
  
  return response;
}

bool ViewPlanner::getViewInformation( std::vector<double>& _output, movements::PoseVector& _poses )
{
  ViewInformationReturn request;
  request.request.call.poses = movements::toROS(_poses);
  request.request.call.metric_names = metrics_to_use_;
  
  request.request.call.ray_resolution_x = 0.5;
  request.request.call.ray_resolution_y = 0.5;
  request.request.call.ray_step_size = 2;
  
  double subwindow_width = 188; // [px]
  double subwindow_height = 120; // [px]
  request.request.call.max_x = 376 + subwindow_width/2;
  request.request.call.min_x = 376 - subwindow_width/2;
  request.request.call.max_y = 240 + subwindow_height/2;
  request.request.call.min_y = 240 - subwindow_height/2;
  
  request.request.call.min_ray_depth = 0.05;
  request.request.call.max_ray_depth = 1.5;
  request.request.call.occupied_passthrough_threshold = 0;
  
  bool response = current_view_retriever_.call(request);
  
  if( response )
  {
    _output = request.response.expected_information.values;
  }
  
  return response;
}

void ViewPlanner::commandCallback( const std_msgs::StringConstPtr& _msg )
{
  if( _msg->data=="START" )
  {
    start_ = true;
    pause_ = false;
    stop_and_print_ = false;
  }
  else if( _msg->data=="PAUSE" )
  {
    start_ = false;
    pause_ = true;
    stop_and_print_ = false;
  }
  else if( _msg->data=="STOP_AND_PRINT" )
  {
    start_ = false;
    pause_ = false;
    stop_and_print_ = true;
  }
  else if( _msg->data=="REINIT" )
  {
    reinit_ = true;
  }
  else if( _msg->data=="ABORT_LOOP" )
  {
    abort_loop_ = true;
  }
  else if( _msg->data=="PRINT_DATA" )
  {
    saveDataToFile();
  }
}


}