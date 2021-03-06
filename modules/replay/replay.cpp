/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
* Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Jan Starzak, jan@ministryofgoodsteps.com
*/

#include "replay.h"

#include "producer/replay_producer.h"
#include "consumer/replay_consumer.h"

#include <core/producer/frame_producer.h>
#include <core/consumer/frame_consumer.h>

#include <common/utility/string.h>

#include <FreeImage.h>
#include <setjmp.h>

namespace caspar { namespace replay {

void init()
{
	core::register_producer_factory(create_producer);
	core::register_consumer_factory([](const std::vector<std::wstring>& params){return create_consumer(params);});
}

std::wstring get_version()
{
	return L"0.1-beta";
}

}}