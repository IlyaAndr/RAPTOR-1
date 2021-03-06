#include <algorithm> // std::find, std::min

#include "raptor.hpp"


// Check if stop1 comes before/after stop2 in the route
bool Raptor::check_stops_order(const route_id_t& route_id, const node_id_t& stop1, const node_id_t& stop2) {
    #ifdef PROFILE
    Profiler prof {__func__};
    #endif

    const auto& route = m_timetable->routes[route_id];

    const auto& idx1 = route.stop_positions.at(stop1);
    const auto& idx2 = route.stop_positions.at(stop2);

    return idx1 < idx2;
}


route_stop_queue_t Raptor::make_queue() {
    #ifdef PROFILE
    Profiler prof {__func__};
    #endif

    route_stop_queue_t queue;

    for (const auto& stop: m_timetable->stops) {
        const auto& stop_id = stop.id;

        if (stop_is_marked[stop_id]) {
            for (const auto& route_id: stop.routes) {
                const auto& route_iter = queue.find(route_id);

                // Check if there is already a pair (r, p) in the queue
                if (route_iter != queue.end()) {
                    const auto& p = route_iter->second;

                    // If s comes before p, replace p by s
                    if (check_stops_order(route_id, stop_id, p)) {
                        queue[route_id] = stop_id;
                    }
                } else {
                    // If r is not in the queue, add (r, s) to the queue
                    queue[route_id] = stop_id;
                }
            }
        }
    }

    stop_is_marked.assign(stop_is_marked.size(), false);

    return queue;
}


// Find the earliest trip in route r that one can catch at stop s in round k,
// i.e., the earliest trip t such that t_dep(t, s) >= t_(k-1) (s),
// or the latest trip t such that t_arr(t, s) <= t_(k-1) (s) if the query is backward
trip_id_t Raptor::earliest_trip(const route_id_t& route_id, const size_t& stop_idx, const Time& t) {
    #ifdef PROFILE
    Profiler prof {__func__};
    #endif

    const auto& stop_events = m_timetable->routes[route_id].stop_times_by_stops[stop_idx];
    const auto& iter = std::lower_bound(stop_events.begin(), stop_events.end(), t,
                                        [&](const StopTime& st, const Time& t) { return st.dep < t; });

    const auto& distance = static_cast<size_t>(iter - stop_events.begin());
    const auto& trip_id = m_timetable->routes[route_id].trips[distance];
    trip_id_t earliest_trip = iter == stop_events.end() ? NULL_TRIP : trip_id;

    return earliest_trip;
}


std::vector<Time> Raptor::query(const node_id_t& source_id, const node_id_t& target_id, const Time& departure_time) {
    std::vector<Time> target_labels;

    // Initialisation
    earliest_arrival_time[source_id] = {departure_time};
    prev_earliest_arrival_time[source_id] = {departure_time};

    stop_is_marked[source_id] = true;

    // If walking is unlimited, we can have a pure walking journey from the source to the target.
    // But in the case of profile queries, we need journeys to contain at least one trip, i.e.,
    // direct walking from s to t is prohibited.
    if (use_hl && !profile) {
        auto target_arrival_time = departure_time + m_timetable->walking_time(source_id, target_id);

        earliest_arrival_time[target_id] = target_arrival_time;
    }

    target_labels.push_back(earliest_arrival_time[target_id]);

    uint16_t round {0};
    while (true) {
        ++round;

        #ifdef PROFILE
        auto* prof_1 = new Profiler {"stage 1"};
        #endif

        // First stage, copy the earliest arrival times to the previous round
        for (const auto& stop: m_timetable->stops) {
            if (stop_is_marked[stop.id]) {
                prev_earliest_arrival_time[stop.id] = earliest_arrival_time[stop.id];
            }
        }

        #ifdef PROFILE
        delete prof_1;
        #endif

        // Second stage
        auto queue = make_queue();
        stops_improved = false;

        #ifdef PROFILE
        auto* prof_2 = new Profiler {"traverse routes"};
        #endif

        // Traverse each route
        for (const auto& route_stop: queue) {
            const auto& route_id = route_stop.first;
            const auto& stop_id = route_stop.second;
            auto& route = m_timetable->routes[route_id];

            trip_id_t t = NULL_TRIP;
            size_t stop_idx = route.stop_positions[stop_id];

            // Iterate over the stops of the route beginning with stop_id
            for (size_t i = stop_idx; i < route.stops.size(); ++i) {
                node_id_t p_i = route.stops[i];
                Time dep, arr;

                if (t != NULL_TRIP) {
                    // Get the position of the trip t
                    trip_pos_t trip_pos = m_timetable->trip_positions[t];
                    size_t pos = trip_pos.second;

                    // Get the departure and arrival time of the trip t at the stop p_i
                    dep = route.stop_times_by_trips[pos][i].dep;
                    arr = route.stop_times_by_trips[pos][i].arr;

                    // Local and target pruning
                    if (arr < std::min(earliest_arrival_time[p_i], earliest_arrival_time[target_id])) {
                        earliest_arrival_time[p_i] = arr;
                        stop_is_marked[p_i] = true;
                        stops_improved = true;
                    }
                }

                // Check if we can catch an earlier trip at p_i
                if (prev_earliest_arrival_time[p_i] <= dep) {
                    t = earliest_trip(route_id, i, prev_earliest_arrival_time[p_i]);
                }
            }
        }

        #ifdef PROFILE
        delete prof_2;
        #endif

        target_labels.push_back(earliest_arrival_time[target_id]);
        if (!stops_improved) break;

        // Third stage, look at footpaths

        // In the first round, we need to consider also the transfers starting from the source,
        // this was not considered in the original version of RAPTOR
        if (round == 1 && !profile) {
            stop_is_marked[source_id] = true;
        }

        scan_footpaths(target_id);

        // After having scanned the transfers/foot paths, we remove source_id
        // from the set of marked stops. Leaving it there would change nothing,
        // as we already marked it in the initialisation step, and scanning the routes
        // starting from source_id again is just a duplication of what was already done
        // in the first round.
        if (round == 1 && !profile) {
            stop_is_marked[source_id] = false;
        }

        // The earliest arrival time at target_id could have been changed
        // after scanning the footpaths, thus we need to update the labels
        // of the target here
        target_labels.back() = earliest_arrival_time[target_id];
    }

    return target_labels;
}


void Raptor::scan_footpaths(const node_id_t& target_id) {
    Time tmp_time;
    static std::unordered_set<node_id_t> improved_stops_or_hubs;

    if (!use_hl) {
        for (const auto& stop: m_timetable->stops) {
            const auto& stop_id = stop.id;

            if (stop_is_marked[stop_id]) {
                for (const auto& transfer: m_timetable->stops[stop_id].transfers) {
                    const auto& dest_id = transfer.dest;
                    const auto& transfer_time = transfer.time;

                    tmp_time = earliest_arrival_time[stop_id] + transfer_time;

                    if (tmp_time < earliest_arrival_time[dest_id]) {
                        earliest_arrival_time[dest_id] = tmp_time;
                        improved_stops_or_hubs.insert(dest_id);
                    }

                    // Since the transfers are sorted in the increasing order of walking time,
                    // we can skip the scanning of the transfers as soon as the arrival time
                    // of the destination is later than that of the target
                    if (tmp_time > earliest_arrival_time[target_id]) break;
                }
            }
        }

        for (const auto& stop_id: improved_stops_or_hubs) {
            stop_is_marked[stop_id] = true;
        }
    } else {
        for (const auto& stop: m_timetable->stops) {
            const auto& stop_id = stop.id;

            if (stop_is_marked[stop_id]) {
                for (const auto& kv: m_timetable->stops[stop_id].out_hubs) {
                    const auto& walking_time = kv.first;
                    const auto& hub_id = kv.second;

                    tmp_time = earliest_arrival_time[stop_id] + walking_time;

                    // Since we sort the links stop->out-hub in the increasing order of walking time,
                    // as soon as the arrival time propagated to a hub is after the earliest arrival time
                    // at the target, there is no need to propagate to the next hubs
                    if (tmp_time > earliest_arrival_time[target_id]) break;

                    if (tmp_time < tmp_hub_labels[hub_id]) {
                        tmp_hub_labels[hub_id] = tmp_time;
                        improved_stops_or_hubs.insert(hub_id);
                    }
                }
            }
        }

        for (const auto& hub_id: improved_stops_or_hubs) {
            // We need to check if hub_id, which is the out-hub of some stop,
            // is the in-hub of some other stop
            if (m_timetable->inverse_in_hubs[hub_id].empty()) {
                for (const auto& kv: m_timetable->inverse_in_hubs[hub_id]) {
                    const auto& walking_time = kv.first;
                    const auto& stop_id = kv.second;

                    tmp_time = tmp_hub_labels[hub_id] + walking_time;
                    if (tmp_time > earliest_arrival_time[target_id]) break;

                    if (tmp_time < earliest_arrival_time[stop_id]) {
                        earliest_arrival_time[stop_id] = tmp_time;
                        stop_is_marked[stop_id] = true;
                    }
                }
            }
        }
    }

    improved_stops_or_hubs.clear();
}


void Raptor::init() {
    stop_is_marked.assign(m_timetable->max_stop_id + 1, false);
    earliest_arrival_time.resize(m_timetable->max_stop_id + 1);
    prev_earliest_arrival_time.resize(m_timetable->max_stop_id + 1);

    if (use_hl) {
        tmp_hub_labels.resize(m_timetable->max_node_id + 1);
    }
}


void Raptor::clear() {
    stop_is_marked.clear();
    earliest_arrival_time.clear();
    prev_earliest_arrival_time.clear();

    if (use_hl) {
        tmp_hub_labels.clear();
    }
}
