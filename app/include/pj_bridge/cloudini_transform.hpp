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

#pragma once

#include "pj_bridge/message_transform.hpp"

namespace pj_bridge {

/// `cloudini` transform: sensor_msgs PointCloud2 -> point_cloud_interfaces
/// CompressedPointCloud2. Works on CDR bytes, so it serves any backend whose
/// PointCloud2 is CDR-encoded. Only declared when Cloudini is available.
///
/// Params: resolution (float, default 0.001), fields ({name: resolution}, 0
/// removes the field), viz_preprocessing (bool, default false).
TransformFactory make_cloudini_transform_factory();

}  // namespace pj_bridge
