/*
 * Copyright (C) 2026 Davide Faconti
 *
 * This file is part of pj_bridge.
 *
 * pj_bridge is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pj_bridge is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with pj_bridge. If not, see <https://www.gnu.org/licenses/>.
 */

#include "pj_bridge_rti/rti_subscription_manager.hpp"

namespace pj_bridge {

RtiSubscriptionManager::RtiSubscriptionManager(DdsSubscriptionManager& inner) : inner_(inner) {}

void RtiSubscriptionManager::set_message_callback(MessageCallback callback) {
  // DdsSubscriptionManager::DdsMessageCallback has the exact same signature as MessageCallback
  inner_.set_message_callback(std::move(callback));
}

bool RtiSubscriptionManager::subscribe(const std::string& topic_name, const std::string& /*topic_type*/) {
  // DDS subscribe only needs topic_name (type is discovered dynamically)
  return inner_.subscribe(topic_name);
}

bool RtiSubscriptionManager::unsubscribe(const std::string& topic_name) {
  return inner_.unsubscribe(topic_name);
}

void RtiSubscriptionManager::unsubscribe_all() {
  inner_.unsubscribe_all();
}

}  // namespace pj_bridge
