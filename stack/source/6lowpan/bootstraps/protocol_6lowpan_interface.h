/*
 * Copyright (c) 2015, 2017, 2019, Pelion and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef PROTOCOL_6LOWPAN_INTERFACE_H_
#define PROTOCOL_6LOWPAN_INTERFACE_H_
#include <stdint.h>

/**
 * 6LoWPAN interface activate
 */
int8_t nwk_6lowpan_up(struct net_if *cur);
/**
 * 6LoWPAN interface deactivate
 */
int8_t nwk_6lowpan_down(struct net_if *cur);


#endif /* PROTOCOL_6LOWPAN_INTERFACE_H_ */
