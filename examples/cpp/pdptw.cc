// Copyright 2010-2018 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//
// Pickup and Delivery Problem with Time Windows.
// The overall objective is to minimize the length of the routes delivering
// quantities of goods between pickup and delivery locations, taking into
// account vehicle capacities and node time windows.
// Given a set of pairs of pickup and delivery nodes, find the set of routes
// visiting all the nodes, such that
// - corresponding pickup and delivery nodes are visited on the same route,
// - the pickup node is visited before the corresponding delivery node,
// - the quantity picked up at the pickup node is the same as the quantity
//   delivered at the delivery node,
// - the total quantity carried by a vehicle at any time is less than its
//   capacity,
// - each node must be visited within its time window (time range during which
//   the node is accessible).
// The maximum number of vehicles used (i.e. the number of routes used) is
// specified in the data but can be overridden using the --pdp_force_vehicles
// flag.
//
// A further description of the problem can be found here:
// http://en.wikipedia.org/wiki/Vehicle_routing_problem
// http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.123.9965&rep=rep1&type=pdf.
// Reads data in the format defined by Li & Lim
// (https://www.sintef.no/projectweb/top/pdptw/li-lim-benchmark/documentation/).

#include <utility>
#include <vector>
#include <unordered_set>
#include <nlohmann/json.hpp>

#include "ortools/base/mathutil.h"
#include "ortools/constraint_solver/routing.h"
#include "ortools/constraint_solver/routing_enums.pb.h"
#include "ortools/constraint_solver/routing_index_manager.h"
#include "ortools/constraint_solver/routing_parameters.h"
#include "ortools/constraint_solver/routing_parameters.pb.h"

#define EARTH_RADIUS (6.3781 * 1e6)

namespace operations_research {

	// Scaling factor used to scale up distances, allowing a bit more precision
	// from Euclidean distances.
	const int64 kScalingFactor = 1000;

	// Vector of (x,y) node coordinates, *unscaled*, in some imaginary planar,
	// metric grid.
	typedef std::vector<std::pair<double, double> > Coordinates;

	const std::pair<double, double> dummy = std::pair<double, double>(-1, -1);

	// Returns the scaled Euclidean distance between two nodes, coords holding the
	// coordinates of the nodes.
	int64 Travel(const Coordinates* const coords,
			RoutingIndexManager::NodeIndex from,
			RoutingIndexManager::NodeIndex to,
			const int64 speed) {
		DCHECK(coords != nullptr);
		const std::pair<double, double> src = coords->at(from.value());
		const std::pair<double, double> dst = coords->at(to.value());
		if (src == dummy || dst == dummy) {
			return 0;
		} else {
			const double xd = (dst.second - src.second) * 
				cos(0.5 * (src.first + dst.first) * M_PI/180) * 
				M_PI/180 * EARTH_RADIUS;
			const double yd = (dst.first - src.first) * M_PI/180 * EARTH_RADIUS;
			return static_cast<int64>(kScalingFactor *
					std::sqrt(1.0L * xd * xd + yd * yd)/speed);
		}
	}

	// Returns the scaled service time at a given node, service_times holding the
	// service times.
	int64 ServiceTime(const std::vector<int64>* const service_times,
			RoutingIndexManager::NodeIndex node) {
		return kScalingFactor * service_times->at(node.value());
	}

	// Returns the scaled (distance plus service time) between two indices, coords
	// holding the coordinates of the nodes and service_times holding the service
	// times.
	// The service time is the time spent to execute a delivery or a pickup.
	int64 TravelPlusServiceTime(const RoutingIndexManager& manager,
			const Coordinates* const coords,
			const std::vector<int64>* const service_times,
			int64 from_index, int64 to_index, int64 speed) {
		const RoutingIndexManager::NodeIndex from = manager.IndexToNode(from_index);
		const RoutingIndexManager::NodeIndex to = manager.IndexToNode(to_index);
		return ServiceTime(service_times, from) + Travel(coords, from, to, speed);
	}

	// Returns the list of variables to use for the Tabu metaheuristic.
	// The current list is:
	// - Total cost of the solution,
	// - Number of used vehicles,
	// - Total schedule duration.
	// TODO(user): add total waiting time.
	std::vector<IntVar*> GetTabuVars(std::vector<IntVar*> existing_vars,
			operations_research::RoutingModel* routing) {
		Solver* const solver = routing->solver();
		std::vector<IntVar*> vars(std::move(existing_vars));
		vars.push_back(routing->CostVar());

		IntVar* used_vehicles = solver->MakeIntVar(0, routing->vehicles());
		std::vector<IntVar*> is_used_vars;
		// Number of vehicle used
		is_used_vars.reserve(routing->vehicles());
		for (int v = 0; v < routing->vehicles(); v++) {
			is_used_vars.push_back(solver->MakeIsDifferentCstVar(
						routing->NextVar(routing->Start(v)), routing->End(v)));
		}
		solver->AddConstraint(
				solver->MakeEquality(solver->MakeSum(is_used_vars), used_vehicles));
		vars.push_back(used_vehicles);

		return vars;
	}

	// Outputs a solution to the current model in a std::string.
	nlohmann::json VerboseOutput(const RoutingModel& routing,
			const RoutingIndexManager& manager,
			const Assignment& assignment,
			const Coordinates& coords,
			const std::vector<int>& request_times,
			const std::vector<int>& customer_ids,
			const std::unordered_set<int> &unique_customers,
			const std::vector<int64>& service_times,
			const std::vector<double>& uw_interests,
			const std::vector<RoutingIndexManager::NodeIndex>& pickups,
			const std::vector<RoutingIndexManager::NodeIndex>& deliveries,
			const int64 speed) {
		
		// store allocation by customer
		std::map<int, int> allocation;

		// prepare JSON output
		nlohmann::json j;
		j["routes"] = {};

		//const RoutingDimension& time_dimension = routing.GetDimensionOrDie("time");
		//const RoutingDimension& load_dimension = routing.GetDimensionOrDie("demand");
		for (int i = 0; i < routing.vehicles(); ++i) {
			int64 index = routing.Start(i);
			nlohmann::json path;
			double total_interest = 0;
			int total_time = 0;
			if (routing.IsEnd(assignment.Value(routing.NextVar(index)))) {
				//output.append("empty");
			} else {
				while (!routing.IsEnd(index)) {
					nlohmann:: json t;
					int64 x = manager.IndexToNode(index).value();
					std::pair<double, double> dst = coords[x];
					t["location"] = {{"latitude", dst.first}, {"longitude", dst.second}};
					t["app_id"] = customer_ids[x];
					t["destination"] = {{"latitude", -1}, {"longitude", -1}};
					
					// credit during pickup
					if (deliveries[x].value() != 0 && pickups[x].value() == 0) {
						t["request_time"] = request_times[x];
						// add destination
						int64 y = deliveries[x].value();
						t["destination"] = {
							{"latitude", coords[y].first},
							{"longitude", coords[y].second}};
						t["interest"] = uw_interests[x];
						total_interest += uw_interests[x];
						allocation[customer_ids[x]] += uw_interests[x];
					} else if (customer_ids[x] != -1) {
						allocation[customer_ids[x]] += 0;
					}
					int64 next_index = assignment.Value(routing.NextVar(index));
					total_time += TravelPlusServiceTime(
							manager, &coords, &service_times, index, 
							next_index, speed);
					t["fulfill_time"] = total_time/operations_research::kScalingFactor;
					index = next_index;
					
					// append to path
					path.push_back(t);
				}
				
				// assemble JSON for vehicle
				nlohmann::json v;
				v["total_interest"] = total_interest;
				v["total_time"] = total_time/operations_research::kScalingFactor;
				v["vehicle_start"] = {
					{"latitude", path[0]["location"]["latitude"]},
					{"longitude", path[0]["location"]["longitude"]}};
				v["vehicle_end"] = {
					{"latitude", path[path.size()-1]["location"]["latitude"]},
					{"longitude", path[path.size()-1]["location"]["longitude"]}};
				v["path"] = {};
				for (int i = 1; i < path.size()-1; ++i) {
					v["path"].push_back(path[i]);
				}
				j["routes"].push_back(v);
			}
		}

		// save allocation
		j["allocation"] = {};
		int total;
		for (const auto& x : unique_customers) {
			if (x == -1) continue;
			std::string key = std::to_string(x);
			j["allocation"][key] = allocation[x];
			total += allocation[x];
		}
		return j;
	}

	namespace {
		// An inefficient but convenient method to parse a whitespace-separated list
		// of doubles. Returns true iff the input std::string was entirely valid and
		// parsed.
		bool SafeParseDoubleArray(const std::string& str,
				std::vector<double>* parsed_dbl) {
			std::istringstream input(str);
			double x;
			parsed_dbl->clear();
			while (input >> x) parsed_dbl->push_back(x);
			return input.eof();
		}
	}  // namespace

	// Builds and solves a model from a file in the format defined by Li & Lim
	// (https://www.sintef.no/projectweb/top/pdptw/li-lim-benchmark/documentation/).
	bool LoadAndSolve(const RoutingModelParameters& model_parameters,
			const RoutingSearchParameters& search_parameters) {
		// Load all the lines of the file in RAM (it shouldn't be too large anyway).
		std::vector<std::string> lines;
		std::string line;
		while (std::getline(std::cin, line)) {
			lines.push_back(line);
		}
		
		// Reading header.
		if (lines.empty()) {
			LOG(WARNING) << "Empty file (stdin)";
			return false;
		}
		// Parse file header.
		std::vector<double> parsed_dbl;
		if (!SafeParseDoubleArray(lines[0], &parsed_dbl) || parsed_dbl.size() != 4 ||
				parsed_dbl[0] < 0 || parsed_dbl[1] < 0 || parsed_dbl[2] < 0 || parsed_dbl[3] < 0) {
			LOG(WARNING) << "Malformed header: " << lines[0];
			return false;
		}
		const int num_vehicles = parsed_dbl[0];
		const int64 capacity = parsed_dbl[1];
		const int64 speed = parsed_dbl[2];
		const int64 horizon = parsed_dbl[3];

		// Parse order data.
		std::vector<int> task_ids;
		std::vector<int> customer_ids;
		std::vector<int> request_times;
		std::vector<std::pair<double, double> > coords;
		std::vector<int64> demands;
		std::vector<double> interests;
		std::vector<double> uw_interests;
		std::vector<int64> service_times;
		std::vector<RoutingIndexManager::NodeIndex> pickups;
		std::vector<RoutingIndexManager::NodeIndex> deliveries;
		std::vector<RoutingIndexManager::NodeIndex> starts;
		std::vector<RoutingIndexManager::NodeIndex> ends;
	
		for (int line_index = 1; line_index < lines.size(); ++line_index) {
			if (!SafeParseDoubleArray(lines[line_index], &parsed_dbl) ||
					parsed_dbl.size() != 11 || parsed_dbl[0] < 0 || parsed_dbl[6] < 0 ||
					parsed_dbl[7] < 0 || parsed_dbl[8] < 0 || parsed_dbl[9] < 0 || 
					parsed_dbl[10] < 0) {
				LOG(WARNING) << "Malformed line #" << line_index << ": "
					<< lines[line_index];
				return false;
			}
			const int task_id = parsed_dbl[0];
			const int customer_id = parsed_dbl[1];
			const int request_time = parsed_dbl[2];
			const double x = parsed_dbl[3];
			const double y = parsed_dbl[4];
			const int64 demand = parsed_dbl[5];
			const double interest = parsed_dbl[6];
			const double uw_interest = parsed_dbl[7];
			const int64 service_time = parsed_dbl[8];
			const int pickup = parsed_dbl[9];
			const int delivery = parsed_dbl[10];
			task_ids.push_back(task_id);
			customer_ids.push_back(customer_id);
			request_times.push_back(request_time);
			coords.push_back(std::make_pair(x, y));
			demands.push_back(demand);
			interests.push_back(interest);
			uw_interests.push_back(uw_interest);
			service_times.push_back(service_time);
			pickups.push_back(RoutingIndexManager::NodeIndex(pickup));
			deliveries.push_back(RoutingIndexManager::NodeIndex(delivery));
			if (pickup == 0 && delivery == 0) {
				starts.push_back(RoutingIndexManager::NodeIndex(task_id));
			}
		}

		// Add dummy node.
		const int task_id = task_ids.size();
		task_ids.push_back(task_id);
		customer_ids.push_back(-1);
		coords.push_back(std::make_pair(-1, -1));
		request_times.push_back(0);
		demands.push_back(0);
		interests.push_back(0);
		uw_interests.push_back(0);
		service_times.push_back(0);
		pickups.push_back(RoutingIndexManager::NodeIndex(0));
		deliveries.push_back(RoutingIndexManager::NodeIndex(0));
		for (int i = 0; i < starts.size(); ++i) {
			ends.push_back(RoutingIndexManager::NodeIndex(task_id));
		}

		// Store unique customer IDs
		std::unordered_set<int> unique_customers(customer_ids.begin(), customer_ids.end());

		// Build pickup and delivery model.
		const int num_nodes = task_ids.size();
		RoutingIndexManager manager(
				num_nodes, 
				num_vehicles, 
				const_cast<const std::vector<RoutingIndexManager::NodeIndex>&>(starts),
				const_cast<const std::vector<RoutingIndexManager::NodeIndex>&>(ends));
		RoutingModel routing(manager, model_parameters);
		const int vehicle_cost =
			routing.RegisterTransitCallback([&coords, &manager, &speed](int64 i, int64 j) {
					return Travel(const_cast<const Coordinates*>(&coords),
							manager.IndexToNode(i), manager.IndexToNode(j),
							speed);
					});
		routing.SetArcCostEvaluatorOfAllVehicles(vehicle_cost);
		RoutingTransitCallback2 demand_evaluator = [&](int64 from_index,
				int64 to_index) {
			return demands[manager.IndexToNode(from_index).value()];
		};
		routing.AddDimension(routing.RegisterTransitCallback(demand_evaluator), 0,
				capacity, /*fix_start_cumul_to_zero=*/true, "demand");
		RoutingTransitCallback2 time_evaluator = [&](int64 from_index,
				int64 to_index) {
			return TravelPlusServiceTime(manager, &coords, &service_times, from_index,
					to_index, speed);
		};
		routing.AddDimension(routing.RegisterTransitCallback(time_evaluator),
				kScalingFactor * horizon, kScalingFactor * horizon,
				/*fix_start_cumul_to_zero=*/true, "time");
		const RoutingDimension& time_dimension = routing.GetDimensionOrDie("time");
		Solver* const solver = routing.solver();
		for (int node = 0; node < num_nodes; ++node) {
			const int64 index =
				manager.NodeToIndex(RoutingIndexManager::NodeIndex(node));
			if (pickups[node] == 0 && deliveries[node] != 0) {
				const int64 delivery_index = manager.NodeToIndex(deliveries[node]);
				solver->AddConstraint(solver->MakeEquality(
							routing.VehicleVar(index), routing.VehicleVar(delivery_index)));
				solver->AddConstraint(
						solver->MakeLessOrEqual(time_dimension.CumulVar(index),
							time_dimension.CumulVar(delivery_index)));
				routing.AddPickupAndDelivery(index,
						manager.NodeToIndex(deliveries[node]));
			}
			//IntVar* const cumul = time_dimension.CumulVar(index);
			//cumul->SetMin(kScalingFactor * open_times[node]);
			//cumul->SetMax(kScalingFactor * close_times[node]);
		}

		if (search_parameters.local_search_metaheuristic() ==
				LocalSearchMetaheuristic::GENERIC_TABU_SEARCH) {
			// Create variable for the total schedule time of the solution.
			// This will be used as one of the Tabu criteria.
			// This is done here and not in GetTabuVarsCallback as it requires calling
			// AddVariableMinimizedByFinalizer and this method must be called early.
			std::vector<IntVar*> end_cumuls;
			std::vector<IntVar*> start_cumuls;
			for (int i = 0; i < routing.vehicles(); ++i) {
				end_cumuls.push_back(time_dimension.CumulVar(routing.End(i)));
				start_cumuls.push_back(time_dimension.CumulVar(routing.Start(i)));
			}
			IntVar* total_time = solver->MakeIntVar(0, 99999999, "total");
			solver->AddConstraint(solver->MakeEquality(
						solver->MakeDifference(solver->MakeSum(end_cumuls),
							solver->MakeSum(start_cumuls)),
						total_time));

			routing.AddVariableMinimizedByFinalizer(total_time);

			RoutingModel::GetTabuVarsCallback tabu_var_callback =
				[total_time](RoutingModel* model) {
					return GetTabuVars({total_time}, model);
				};
			routing.SetTabuVarsCallback(tabu_var_callback);
		}

		// Adding penalty costs to allow skipping orders.
		const int64 kPenalty = 10000000;
		for (RoutingIndexManager::NodeIndex order(1); order < routing.nodes();
				++order) {
			int64 node = manager.NodeToIndex(order);
			if (node == RoutingIndexManager::kUnassigned) {
				continue;
			}
			std::vector<int64> orders(1, node);
			routing.AddDisjunction(orders, kPenalty * abs(interests[order.value()]));
		}

		// Solve pickup and delivery problem.
		const Assignment* assignment = routing.SolveWithParameters(search_parameters);
		
		if (nullptr != assignment) {
			nlohmann::json x = VerboseOutput(routing, manager, *assignment, coords,
					request_times, customer_ids, unique_customers, 
					service_times, uw_interests, pickups, deliveries, speed);
			std::cout << x;
			return true;
		}
		return false;
	}

}  // namespace operations_research

int main(int argc, char** argv) {
	operations_research::RoutingModelParameters model_parameters =
		operations_research::DefaultRoutingModelParameters();
	model_parameters.set_reduce_vehicle_cost_model(true);
	operations_research::RoutingSearchParameters search_parameters =
		operations_research::DefaultRoutingSearchParameters();
	search_parameters.mutable_time_limit()->set_seconds(180);
	if (!operations_research::LoadAndSolve(model_parameters, search_parameters)) {
		LOG(INFO) << "Error solving model.";
	}
	return EXIT_SUCCESS;
}
