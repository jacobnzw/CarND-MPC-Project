#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "MPC.h"
#include "json.hpp"

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s)
{
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.rfind("}]");
  if (found_null != string::npos)
  {
    return "";
  }
  else if (b1 != string::npos && b2 != string::npos)
  {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

// Evaluate a polynomial.
double polyeval(Eigen::VectorXd coeffs, double x)
{
  double result = 0.0;
  for (int i = 0; i < coeffs.size(); i++)
  {
    result += coeffs[i] * pow(x, i);
  }
  return result;
}

// Fit a polynomial.
// Adapted from
// https://github.com/JuliaMath/Polynomials.jl/blob/master/src/Polynomials.jl#L676-L716
Eigen::VectorXd polyfit(Eigen::VectorXd xvals, Eigen::VectorXd yvals, int order)
{
  assert(xvals.size() == yvals.size());
  assert(order >= 1 && order <= xvals.size() - 1);
  Eigen::MatrixXd A(xvals.size(), order + 1);

  for (int i = 0; i < xvals.size(); i++)
  {
    A(i, 0) = 1.0;
  }

  for (int j = 0; j < xvals.size(); j++)
  {
    for (int i = 0; i < order; i++)
    {
      A(j, i + 1) = A(j, i) * xvals(j);
    }
  }

  auto Q = A.householderQr();
  auto result = Q.solve(yvals);
  return result;
}

int main()
{
  uWS::Hub h;

  // MPC is initialized here!
  MPC mpc;

  // storing last control inputs for latency compensation
  double last_a = 0;
  double last_delta = 0;

  h.onMessage([&mpc, &last_a, &last_delta](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length, uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    string sdata = string(data).substr(0, length);
    cout << sdata << endl;
    if (sdata.size() > 2 && sdata[0] == '4' && sdata[1] == '2')
    {
      string s = hasData(sdata);
      if (s != "")
      {
        auto j = json::parse(s);
        string event = j[0].get<string>();
        if (event == "telemetry")
        {
          // j[1] is the data JSON object
          vector<double> ptsx = j[1]["ptsx"];   // x-coordinates of the waypoints in map frame
          vector<double> ptsy = j[1]["ptsy"];   // y-coordinates ...
          double px = j[1]["x"];      // x-coordinate of the car in map frame
          double py = j[1]["y"];      // y-coordinate ...
          double psi = j[1]["psi"];   // car's heading angle
          double v = j[1]["speed"];   // car's speed in mph

          const short MPC_DT = 0.1;  // actuator latency in seconds
          const double Lf = 2.67;
          // account for actuator latency by projecting the state MPC_DT miliseconds into the future
          px += v*cos(psi)*MPC_DT;
          py += v*sin(psi)*MPC_DT;
          psi += v/Lf*last_delta*MPC_DT;
          v += last_a*MPC_DT;

          // Transform waypoints from map frame to car frame
          for (unsigned int i = 0; i < ptsx.size(); ++i)
          {
            double shift_x = ptsx[i] - px;
            double shift_y = ptsy[i] - py;
            ptsx[i] = shift_x * cos(-psi) - shift_y * sin(-psi);
            ptsy[i] = shift_x * sin(-psi) + shift_y * cos(-psi);
          }
          Eigen::VectorXd x = Eigen::Map<Eigen::VectorXd, Eigen::Unaligned>(ptsx.data(), ptsx.size());
          Eigen::VectorXd y = Eigen::Map<Eigen::VectorXd, Eigen::Unaligned>(ptsy.data(), ptsy.size());
          // Fit waypoint trajectory using a 3-rd degree polynomial in car frame
          auto coeff = polyfit(x, y, 3);

          // Create initial state
          // car's position [x, y] and heading psi in car frame are always = 0
          double cte = polyeval(coeff, 0);
          double epsi = -atan(coeff[1]);    // f'(x) = 3*a_3*x^2 + 2*a_2*x + a_1  ==> f'(0) = a_1
          Eigen::VectorXd state(6);
          state << 0, 0, 0, v, cte, epsi;

          // account for actuator latency by projecting the state MPC_DT miliseconds into the future
          // state[0] += v*cos(state[2])*MPC_DT;
          // state[1] += v*sin(state[2])*MPC_DT;
          // state[2] += v/Lf*0*MPC_DT;
          // state[3] += 0*MPC_DT;
          
          // Calculate steering angle and throttle using MPC.
          vector<double> soln = mpc.Solve(state, coeff);

          json msgJson;
          // NOTE: Remember to divide by deg2rad(25) before you send the steering value back.
          // Otherwise the values will be in between [-deg2rad(25), deg2rad(25] instead of [-1, 1].
          msgJson["steering_angle"] = soln[0] / (deg2rad(25)*Lf);;
          msgJson["throttle"] = soln[1];

          // save control inputs for latency compensation
          last_delta = msgJson["steering_angle"];
          last_a = msgJson["throttle"];

          //Display the MPC predicted trajectory
          vector<double> mpc_x_vals;
          vector<double> mpc_y_vals;
          //.. add (x,y) points to list here, points are in reference to the vehicle's coordinate system
          // the points in the simulator are connected by a Green line
          for (unsigned int i = 2; i < soln.size(); ++i)
          {
            if (i % 2 == 0)
            {
              mpc_x_vals.push_back(soln[i]);
            }
            else
            {
              mpc_y_vals.push_back(soln[i]);
            }
          }
          msgJson["mpc_x"] = mpc_x_vals;
          msgJson["mpc_y"] = mpc_y_vals;

          //Display the waypoints/reference line
          vector<double> next_x_vals;
          vector<double> next_y_vals;
          //.. add (x,y) points to list here, points are in reference to the vehicle's coordinate system
          // the points in the simulator are connected by a Yellow line
          double poly_inc = 2.5;
          unsigned int num_points = 25;
          for (unsigned int i = 1; i < num_points; ++i)
          {
            next_x_vals.push_back(poly_inc * i);
            next_y_vals.push_back(polyeval(coeff, poly_inc * i));
          }
          msgJson["next_x"] = next_x_vals;
          msgJson["next_y"] = next_y_vals;

          auto msg = "42[\"steer\"," + msgJson.dump() + "]";
          // std::cout << msg << std::endl;
          
          // Artificial latency: simulates communication delays in the car network.
          // NOTE: REMEMBER TO SET THIS TO 100 MILLISECONDS BEFORE SUBMITTING.
          this_thread::sleep_for(chrono::milliseconds(1000*MPC_DT));
          ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }
      }
      else
      {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the
  // program
  // doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1)
    {
      res->end(s.data(), s.length());
    }
    else
    {
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
  if (h.listen(port))
  {
    std::cout << "Listening to port " << port << std::endl;
  }
  else
  {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}
