#include <fstream>
#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "json.hpp"
#include "spline.h"

using namespace std;

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.find_first_of("}");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

double distance(double x1, double y1, double x2, double y2)
{
	return sqrt((x2-x1)*(x2-x1)+(y2-y1)*(y2-y1));
}
int ClosestWaypoint(double x, double y, const vector<double> &maps_x, const vector<double> &maps_y)
{

	double closestLen = 100000; //large number
	int closestWaypoint = 0;

	for(int i = 0; i < maps_x.size(); i++)
	{
		double map_x = maps_x[i];
		double map_y = maps_y[i];
		double dist = distance(x,y,map_x,map_y);
		if(dist < closestLen)
		{
			closestLen = dist;
			closestWaypoint = i;
		}

	}

	return closestWaypoint;

}

int NextWaypoint(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y)
{

	int closestWaypoint = ClosestWaypoint(x,y,maps_x,maps_y);

	double map_x = maps_x[closestWaypoint];
	double map_y = maps_y[closestWaypoint];

	double heading = atan2((map_y-y),(map_x-x));

	double angle = fabs(theta-heading);
  angle = min(2*pi() - angle, angle);

  if(angle > pi()/4)
  {
    closestWaypoint++;
  if (closestWaypoint == maps_x.size())
  {
    closestWaypoint = 0;
  }
  }

  return closestWaypoint;
}

// Transform from Cartesian x,y coordinates to Frenet s,d coordinates
vector<double> getFrenet(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y)
{
	int next_wp = NextWaypoint(x,y, theta, maps_x,maps_y);

	int prev_wp;
	prev_wp = next_wp-1;
	if(next_wp == 0)
	{
		prev_wp  = maps_x.size()-1;
	}

	double n_x = maps_x[next_wp]-maps_x[prev_wp];
	double n_y = maps_y[next_wp]-maps_y[prev_wp];
	double x_x = x - maps_x[prev_wp];
	double x_y = y - maps_y[prev_wp];

	// find the projection of x onto n
	double proj_norm = (x_x*n_x+x_y*n_y)/(n_x*n_x+n_y*n_y);
	double proj_x = proj_norm*n_x;
	double proj_y = proj_norm*n_y;

	double frenet_d = distance(x_x,x_y,proj_x,proj_y);

	//see if d value is positive or negative by comparing it to a center point

	double center_x = 1000-maps_x[prev_wp];
	double center_y = 2000-maps_y[prev_wp];
	double centerToPos = distance(center_x,center_y,x_x,x_y);
	double centerToRef = distance(center_x,center_y,proj_x,proj_y);

	if(centerToPos <= centerToRef)
	{
		frenet_d *= -1;
	}

	// calculate s value
	double frenet_s = 0;
	for(int i = 0; i < prev_wp; i++)
	{
		frenet_s += distance(maps_x[i],maps_y[i],maps_x[i+1],maps_y[i+1]);
	}

	frenet_s += distance(0,0,proj_x,proj_y);

	return {frenet_s,frenet_d};

}

// Transform from Frenet s,d coordinates to Cartesian x,y
vector<double> getXY(double s, double d, const vector<double> &maps_s, const vector<double> &maps_x, const vector<double> &maps_y)
{
	int prev_wp = -1;

	while(s > maps_s[prev_wp+1] && (prev_wp < (int)(maps_s.size()-1) ))
	{
		prev_wp++;
	}

	int wp2 = (prev_wp+1)%maps_x.size();

	double heading = atan2((maps_y[wp2]-maps_y[prev_wp]),(maps_x[wp2]-maps_x[prev_wp]));
	// the x,y,s along the segment
	double seg_s = (s-maps_s[prev_wp]);

	double seg_x = maps_x[prev_wp]+seg_s*cos(heading);
	double seg_y = maps_y[prev_wp]+seg_s*sin(heading);

	double perp_heading = heading-pi()/2;

	double x = seg_x + d*cos(perp_heading);
	double y = seg_y + d*sin(perp_heading);

	return {x,y};

}

double lane_cost_function(double dist,double speed)
{
  // cost function which takes into account speed and distance
  double cost_tmp = 0;
  cost_tmp = exp(-1/dist)+exp(1/speed);
  if(dist < 20 && dist > -5)
  {
   cost_tmp = 999;
  }
  return cost_tmp;
 }


int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";
  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;

  ifstream in_map_(map_file_.c_str(), ifstream::in);

  string line;
  while (getline(in_map_, line)) {
  	istringstream iss(line);
  	double x;
  	double y;
  	float s;
  	float d_x;
  	float d_y;
  	iss >> x;
  	iss >> y;
  	iss >> s;
  	iss >> d_x;
  	iss >> d_y;
  	map_waypoints_x.push_back(x);
  	map_waypoints_y.push_back(y);
  	map_waypoints_s.push_back(s);
  	map_waypoints_dx.push_back(d_x);
  	map_waypoints_dy.push_back(d_y);
  }

  // starting lane number (0 is far left)
  int lane = 1;

  // max speed and target velocity
  double max_speed = 47.5;
  double ref_v = 0.0; 

  h.onMessage([&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,&map_waypoints_dx,&map_waypoints_dy, &lane, &max_speed, &ref_v](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
                     uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    //auto sdata = string(data).substr(0, length);
    //cout << sdata << endl;
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);
        
        string event = j[0].get<string>();
        
        if (event == "telemetry") {
            // j[1] is the data JSON object
          
        	// Main car's localization Data
          	double car_x = j[1]["x"];
          	double car_y = j[1]["y"];
          	double car_s = j[1]["s"];
          	double car_d = j[1]["d"];
          	double car_yaw = j[1]["yaw"];
          	double car_speed = j[1]["speed"];

          	// Previous path data given to the Planner
          	auto previous_path_x = j[1]["previous_path_x"];
          	auto previous_path_y = j[1]["previous_path_y"];
          	// Previous path's end s and d values 
          	double end_path_s = j[1]["end_path_s"];
          	double end_path_d = j[1]["end_path_d"];


            int prev_size = previous_path_x.size();

          	// Sensor Fusion Data, a list of all other cars on the same side of the road.
          	auto sensor_fusion = j[1]["sensor_fusion"];

          	if(prev_size > 0){
              		car_s = end_path_s;
            	}

	    	bool too_close = false;

            //setting up the cost variables
            double left_space;
            double right_space;
            double current_space;

            double left_cost;
            double left_car_cost;
            double right_cost;
            double right_car_cost;
            double current_cost;
            double current_car_cost = 0;
            double follow_speed;
            double follow_distance;

            current_cost = 0;
            left_cost = 1;
            right_cost = 1;

		    // iterates over all vehicles that were detected by sensors
		    // todo filter for onl relevant cars for example closest car to the front and to the
		    // back in each lane
            for(int i = 0; i < sensor_fusion.size(); i ++)
            {

              float d = sensor_fusion[i][6];

              double vx = sensor_fusion[i][3];
              double vy = sensor_fusion[i][4];
              double speed = sqrt(vx*vx + vy*vy);
              double check_car_s = sensor_fusion[i][5];

              // check for car in current lane
              if(d < (2+4*lane+2) && d > (2+4*lane-2))
              {

                // future location of car
                check_car_s += ((double)prev_size*.02*speed);

                current_space = check_car_s - car_s;
                current_car_cost = lane_cost_function(current_space, speed);

                // Store the highest cost
                if(current_car_cost>current_cost)
                {current_cost = current_car_cost;}

                // a cost of 999 is assigned for non-viable lanes
                if(current_cost == 999)
                {
                 // the current lanes cost of 999 is adjusted to 900 to force a no lane change
                 // into a 999 lane
                 current_cost = 900;
                }

                // check gap between my car and car infront
                if ((check_car_s > car_s) && ((check_car_s - car_s) < 20))
                {
                  too_close = true;
                  // set follow speed to prevent constant acc dec
                  // this is not working yet, a possible solution is to do a smoothed speed change
                  // based on the circumstances
                  follow_speed = speed;
                  follow_distance = check_car_s - car_s;
                }
              }

              //check if car in lane to the left
              if(lane>0)
              {
                  if(d < (2+4*(lane-1)+2) && d > (2+4*(lane-1)-2))
                  {
                    check_car_s += ((double)prev_size*.02*speed);

                    left_space = check_car_s - car_s;
                    left_car_cost = lane_cost_function(left_space, speed);
                    //cout << left_car_cost << endl;

                    //only store the highest cost
                    if(left_car_cost > left_cost)
                    {left_cost = left_car_cost;}
                  }
              }  else { left_cost = 999;}

              //check if car in lane to the right
              if(lane<2)
              {
                  if(d < (2+4*(lane+1)+2) && d > (2+4*(lane+1)-2))
                  {
                    check_car_s += ((double)prev_size*.02*speed);

                    right_space = check_car_s - car_s;
                    right_car_cost = lane_cost_function(right_space, speed);

                    if(right_car_cost > right_cost)
                    {right_cost = right_car_cost;}
                  }
              }  else {right_cost = 999;}
            }

          //Decide on lane

          // debug the cost function
          cout << "CurrentCost: " << current_cost << " LeftCost: " << left_cost << " RightCost: "
           << right_cost << endl;

          // Lane change action is determined by the cost function with a preference towards a
          // lane change to the left
          if(left_cost < (current_cost+0.01) && left_cost < right_cost+0.02)
          {
           lane = lane - 1;

           // only one lane change at a time
           if(lane + 2 < car_d/4)
           {lane += 1;}

           cout << "LCL" << endl;
          } else if(right_cost < current_cost-0.02)
          {
           lane = lane + 1;

           if(lane - 1 > car_d/4)
           {lane -= 1;}
           cout << "LCR " << car_d << endl;
          }

          // or stay in current lane

          // do speed adjustments

          if(too_close)
          {
            // slow down
            ref_v -= 0.28;
            if(ref_v < follow_speed && follow_distance > 15)
            {
                ref_v += 0.1;
                if (ref_v > follow_speed)
                {
                ref_v = follow_speed;
                }
            }
          }

            // accelerate to speed limit
          else if(ref_v < max_speed)
          {
            ref_v += 0.22;
          }

          if(ref_v > max_speed)
          {ref_v -= 0.05;}


          // create waypoints
          vector<double> ptsx;
          vector<double> ptsy;

          // ref x, y and yaw
          double ref_x = car_x;
          double ref_y = car_y;
          double ref_yaw = deg2rad(car_yaw);

          // at start, when only have an angle and location
          if (prev_size < 2)
          {
            double prev_car_x = car_x - cos(car_yaw);
            double prev_car_y = car_y - sin(car_yaw);

            // push x
            ptsx.push_back(prev_car_x);
            ptsx.push_back(car_x);

            // push y
            ptsy.push_back(prev_car_y);
            ptsy.push_back(car_y);
          }
          // else use prvious path as reference
        else
        {
          ref_x = previous_path_x[prev_size - 1];
          ref_y = previous_path_y[prev_size - 1];

          double ref_x_prev = previous_path_x[prev_size - 2];
          double ref_y_prev = previous_path_y[prev_size - 2];

          ref_yaw = atan2(ref_y - ref_y_prev, ref_x - ref_x_prev);

          ptsx.push_back(ref_x_prev);
          ptsx.push_back(ref_x);

          ptsy.push_back(ref_y_prev);
          ptsy.push_back(ref_y);

        }

        // add evently 30m spaced point ahead of starting reference
        vector<double> next_wp0 = getXY(car_s + 30, 2 + 4* lane, map_waypoints_s, map_waypoints_x, map_waypoints_y);
        vector<double> next_wp1 = getXY(car_s + 60, 2 + 4* lane, map_waypoints_s, map_waypoints_x, map_waypoints_y);
        vector<double> next_wp2 = getXY(car_s + 90, 2 + 4* lane, map_waypoints_s, map_waypoints_x, map_waypoints_y);

        ptsx.push_back(next_wp0[0]);
        ptsx.push_back(next_wp1[0]);
        ptsx.push_back(next_wp2[0]);

        ptsy.push_back(next_wp0[1]);
        ptsy.push_back(next_wp1[1]);
        ptsy.push_back(next_wp2[1]);


        for (int i = 0; i < ptsx.size(); i++)
        {
          // shift car reference angle to 0 degrees, makes maths easier
          double shift_x = ptsx[i] - ref_x;
          double shift_y = ptsy[i] - ref_y;

          ptsx[i] = (shift_x * cos(0- ref_yaw) - shift_y * sin(0 - ref_yaw));
          ptsy[i] = (shift_x * sin(0- ref_yaw) + shift_y * cos(0 - ref_yaw));

        }

        // create spline
        tk::spline s;

        s.set_points(ptsx, ptsy);

        vector<double> next_x_vals;
        vector<double> next_y_vals;

        // populate next vals with remaining points
        for (int i = 0; i < prev_size; i++)
        {
          next_x_vals.push_back(previous_path_x[i]);
          next_y_vals.push_back(previous_path_y[i]);
        }


        double target_x = 30.0;
        double target_y = s(target_x);
        double target_dist = sqrt((target_x)*(target_x) + (target_y)*(target_y));

        double x_addon = 0;



        // fill up rest of path planner
        for(int i = 1; i <= 50 - prev_size; i ++)
        {
          double N = (target_dist/(.02*ref_v/2.24));
          double x_point = x_addon+(target_x)/N;
          double y_point = s(x_point);

          x_addon = x_point;

          double x_ref = x_point;
          double y_ref = y_point;

          // rotate back to normal plane
          x_point = (x_ref * cos(ref_yaw) - y_ref*sin(ref_yaw));
          y_point = (x_ref * sin(ref_yaw) + y_ref*cos(ref_yaw));

          x_point += ref_x;
          y_point += ref_y;

          next_x_vals.push_back(x_point);
          next_y_vals.push_back(y_point);


        }



          	json msgJson;


          	msgJson["next_x"] = next_x_vals;
          	msgJson["next_y"] = next_y_vals;

          	auto msg = "42[\"control\","+ msgJson.dump()+"]";

          	//this_thread::sleep_for(chrono::milliseconds(1000));
          	ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
          
        } else
        {
            // Manual driving
            std::string msg = "42[\"manual\",{}]";
            ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the
  // program
  // doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}
