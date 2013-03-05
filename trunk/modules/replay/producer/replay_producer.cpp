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
* Author: Robert Nagy, ronag89@gmail.com
*		  Jan Starzak, jan@ministryofgoodsteps.com
*/

#include "replay_producer.h"

#include <core/video_format.h>

#include <core/producer/frame/basic_frame.h>
#include <core/producer/frame/frame_factory.h>
#include <core/mixer/write_frame.h>
#include <common/utility/string.h>

#include <common/env.h>
#include <common/log/log.h>
#include <common/diagnostics/graph.h>

#include <boost/assign.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/thread.hpp>
#include <boost/thread/locks.hpp>
#include <boost/regex.hpp>
#include <boost/timer.hpp>

#include <tbb/concurrent_queue.h>

#include <algorithm>

#include <sys/stat.h>
#include <math.h>

#include "../util/frame_operations.h"
#include "../util/file_operations.h"

#include <Windows.h>

using namespace boost::assign;

namespace caspar { namespace replay {

struct replay_producer : public core::frame_producer
{	
	const std::wstring						filename_;
	safe_ptr<core::basic_frame>				frame_;
	boost::shared_ptr<FILE>					in_file_;
	boost::shared_ptr<FILE>					in_idx_file_;
	boost::shared_ptr<mjpeg_file_header>	index_header_;
	safe_ptr<core::frame_factory>			frame_factory_;
	tbb::atomic<uint64_t>					framenum_;
	tbb::atomic<uint64_t>					result_framenum_;
	uint8_t*								last_field_;
	size_t									last_field_size_;
	float									left_of_last_field_;
	bool									interlaced_;
	float									speed_;
	float									abs_speed_;
	int										frame_divider_;
	int										frame_multiplier_;
	bool									reverse_;
	bool									seeked_;
	const safe_ptr<diagnostics::graph>		graph_;

#define REPLAY_SEMI_SLOW_DISTANCE			12.0f

	explicit replay_producer(const safe_ptr<core::frame_factory>& frame_factory, const std::wstring& filename, const int sign, const long long start_frame) 
		: filename_(filename)
		, frame_(core::basic_frame::empty())
		, frame_factory_(frame_factory)
	{
		in_file_ = safe_fopen(narrow(filename_).c_str(), "rb", _SH_DENYNO);
		if (in_file_ != NULL)
		{
			_off_t size = 0;
			struct stat st;
			in_idx_file_ = safe_fopen(narrow(boost::filesystem::wpath(filename_).replace_extension(L".idx").string()).c_str(), "rb", _SH_DENYNO);
			if (in_idx_file_ != NULL)
			{
				while (size == 0)
				{
					stat(narrow(boost::filesystem::wpath(filename_).replace_extension(L".idx").string()).c_str(), &st);
					size = st.st_size;

					if (size > 0) {
						mjpeg_file_header* header;
						read_index_header(in_idx_file_, &header);
						index_header_ = boost::shared_ptr<mjpeg_file_header>(header);
						CASPAR_LOG(info) << print() << L" File starts at: " << boost::posix_time::to_iso_wstring(index_header_->begin_timecode);

						if (index_header_->field_mode == caspar::core::field_mode::progressive)
						{
							interlaced_ = false;
						}
						else
						{
							interlaced_ = true;
						}

						set_playback_speed(1.0f);
						result_framenum_ = 0;
						framenum_ = 0;
						left_of_last_field_ = 0;

						last_field_ = NULL;

						seeked_ = false;

						if (start_frame > 0) {
							long long frame_pos;
							if (interlaced_)
								frame_pos = (long long)(start_frame * 2.0);
							else
								frame_pos = (long long)start_frame;
				
							seek(frame_pos, sign);
						}

						graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
						graph_->set_text(print());
						diagnostics::register_graph(graph_);
					}
					else
					{
						CASPAR_LOG(warning) << print() << L" Waiting for index file to grow.";
						boost::this_thread::sleep(boost::posix_time::milliseconds(10));
					}
				}
			}
			else
			{
				CASPAR_LOG(error) << print() << L" Index file " << boost::filesystem::wpath(filename_).replace_extension(L".idx").string() << " not found";
				throw file_not_found();
			}
		}
		else
		{
			CASPAR_LOG(error) << print() << L" Video essence file " << filename_ << " not found";
			throw file_not_found();
		}
		//frame_factory_ = frame_factory;
	}
	
	caspar::safe_ptr<core::basic_frame> make_frame(uint8_t* frame_data, size_t size, size_t width, size_t height, bool drop_first_line)
	{
		core::pixel_format_desc desc;
		desc.pix_fmt = core::pixel_format::bgra;
		desc.planes.push_back(core::pixel_format_desc::plane(width, height, 4));
		auto frame = frame_factory_->create_frame(this, desc);
		if (!drop_first_line)
		{
			std::copy_n(frame_data, size, frame->image_data().begin());
		}
		else
		{
			size_t line = width * 4;
			std::copy_n(frame_data + line, size - line, frame->image_data().begin());
		}
		frame->commit();
		frame_ = std::move(frame);
		return frame_;
	}

	long long length_index()
	{
		struct stat st;
		stat(narrow(boost::filesystem::wpath(filename_).replace_extension(L".idx").string()).c_str(), &st);
		_off_t size = st.st_size;
		int el_size = (size - sizeof(mjpeg_file_header)) / sizeof(long long);
		return el_size;
	}

	virtual boost::unique_future<std::wstring> call(const std::wstring& param) override
	{
		boost::promise<std::wstring> promise;
		promise.set_value(do_call(param));
		return promise.get_future();
	}

	std::wstring do_call(const std::wstring& param)
	{
		static const boost::wregex speed_exp(L"SPEED\\s+(?<VALUE>[\\d.-]+)", boost::regex::icase);
		static const boost::wregex pause_exp(L"PAUSE", boost::regex::icase);
		static const boost::wregex seek_exp(L"SEEK\\s+(?<SIGN>[+-|])?(?<VALUE>\\d+)", boost::regex::icase);
		
		boost::wsmatch what;
		if(boost::regex_match(param, what, pause_exp))
		{
			set_playback_speed(0.0f);
			return L"";
		}
		if(boost::regex_match(param, what, speed_exp))
		{
			if(!what["VALUE"].str().empty())
			{
				float speed = boost::lexical_cast<float>(narrow(what["VALUE"].str()).c_str());
				set_playback_speed(speed);
			}
			return L"";
		}
		if(boost::regex_match(param, what, seek_exp))
		{
			int sign = 0;
			if(!what["SIGN"].str().empty())
			{
				if (what["SIGN"].str() == L"+")
					sign = 1;
				else if (what["SIGN"].str() == L"|")
					sign = -2;
				else
					sign = -1;
			}
			if(!what["VALUE"].str().empty())
			{
				double position = boost::lexical_cast<double>(narrow(what["VALUE"].str()).c_str());
				long long frame_pos = 0;
				if (interlaced_)
					frame_pos = (long long)(position * 2.0);
				else
					frame_pos = (long long)position;
				
				seek(frame_pos, sign);
			}
			return L"";
		}

		BOOST_THROW_EXCEPTION(invalid_argument());
	}

	void seek(long long frame_pos, int sign)
	{
		if (sign == 0)
		{
			framenum_ = frame_pos;
			seek_index(in_idx_file_, frame_pos, SEEK_SET);
		}
		else if (sign == -2)
		{
			framenum_ = length_index() - frame_pos - 4;
			seek_index(in_idx_file_, framenum_, SEEK_SET);
		}
		else
		{
			framenum_ = framenum_ + (sign * frame_pos);
			seek_index(in_idx_file_, (frame_pos) * sign, SEEK_CUR);
		}
		seeked_ = true;
	}

	void set_playback_speed(float speed)
	{
		speed_ = speed;
		abs_speed_ = abs(speed);
		if (speed != 0.0f)
			frame_divider_ = abs((int)(1.0f / speed));
		else 
			frame_divider_ = 0;
		frame_multiplier_ = abs((int)(speed));
		reverse_ = (speed >= 0.0f) ? false : true;
	}

	void update_diag(double elapsed)
	{
		graph_->set_text(print());
		graph_->set_value("frame-time", elapsed*0.5);
	}

	void move_to_next_frame()
	{
		if ((reverse_) && (framenum_ > 0))
		{
			framenum_ -= -(frame_multiplier_ > 1 ? frame_multiplier_ : 1);
			seek_index(in_idx_file_, -1 - (frame_multiplier_ > 1 ? frame_multiplier_ : 1), SEEK_CUR);
		}
		else
		{
			framenum_ += (frame_multiplier_ > 1 ? frame_multiplier_ : 1);
			if (frame_multiplier_ > 1)
			{
				seek_index(in_idx_file_, frame_multiplier_, SEEK_CUR);
			}
		}
	}

	void sync_to_frame()
	{
		if (index_header_->field_mode != caspar::core::field_mode::progressive)
		{
			if (
			   ((framenum_ % 2 != 0))
			)
			{
				//CASPAR_LOG(warning) << L" Frame number was " << framenum_ << L", syncing to First Field";
				(void) read_index(in_idx_file_);
				framenum_++;
			}
		}
	}

	void proper_interlace(const mmx_uint8_t* field1, const mmx_uint8_t* field2, mmx_uint8_t* dst)
	{
		if (index_header_->field_mode == caspar::core::field_mode::lower)
		{
			interlace_fields(field2, field1, dst, index_header_->width, index_header_->height, 4);
		}
		else
		{
			interlace_fields(field1, field2, dst, index_header_->width, index_header_->height, 4);
		}
	}

	float modf(float a)
	{
		int intpart = (int)a;
		float decpart = a - intpart;
		return decpart;
	}

	virtual safe_ptr<core::basic_frame> receive(int hint)
	{
		boost::timer frame_timer;

		// IF is paused
		if (speed_ == 0.0f) 
		{
			if (!seeked_)
			{
				result_framenum_++;
				update_diag(frame_timer.elapsed());
				return frame_;
			}
			else
			{
				seeked_ = false;
			}
		}
		// IF speed is less than 0.5x and it's not time for a new frame
		else if (abs_speed_ < 0.5f)
		{
			if (result_framenum_ % frame_divider_ > 0)
			{
				result_framenum_++;
				update_diag(frame_timer.elapsed());
				return frame_; // Return previous frame
			}
		}
		else if ((abs_speed_ > 0.5f) && (abs_speed_ < 1.0f) && (interlaced_))
		{
			// There is no last_field_ to base new field on
			if (last_field_ == NULL)
			{
				long long field1_pos = read_index(in_idx_file_);

				if (field1_pos == -1)
				{	// There are no more frames
					result_framenum_++;
					update_diag(frame_timer.elapsed());
					return frame_;
				}

				move_to_next_frame();

				seek_frame(in_file_, field1_pos, SEEK_SET);

				mmx_uint8_t* field1;
				size_t field1_width;
				size_t field1_height;
				size_t field1_size = read_frame(in_file_, &field1_width, &field1_height, &field1);

				long long field2_pos = read_index(in_idx_file_);

				move_to_next_frame();

				seek_frame(in_file_, field2_pos, SEEK_SET);

				mmx_uint8_t* field2;
				size_t field2_size = read_frame(in_file_, &field1_width, &field1_height, &field2);

				mmx_uint8_t* field_blend = new mmx_uint8_t[field1_size];

				float level = (1 - abs_speed_) * 2;
				blend_images(field1, field2, field_blend, field1_width, field1_height, 4, (uint8_t)(level * 63.0));
				left_of_last_field_ = modf(abs_speed_ * 2);

				mmx_uint8_t* full_frame = new mmx_uint8_t[field1_size * 2];
				proper_interlace(field1, field_blend, full_frame);

				result_framenum_++;

				make_frame(full_frame, field1_size * 2, index_header_->width, index_header_->height, false);

				last_field_ = field2;
				last_field_size_ = field2_size;
				delete field_blend;
				delete full_frame;
				delete field1;

				update_diag(frame_timer.elapsed());

				return frame_;
			}
			else
			{
				float left_in_field1 = 0;
				float left_in_field2 = 0;
				// last_field_, last_field_size_, left_of_last_field_
				left_in_field1 += left_of_last_field_;

				mmx_uint8_t* field1 = last_field_;
				mmx_uint8_t* field2 = NULL;
				size_t field1_size = last_field_size_;

				if (left_in_field1 > 0.5f)
				{
					left_in_field2 = left_in_field1 - 0.5f;
					field1 = new mmx_uint8_t[field1_size];
					memcpy(field1, last_field_, field1_size);
				}
				else
				{
					long long field2_pos = read_index(in_idx_file_);

					if (field2_pos == -1)
					{	// There are no more frames
						result_framenum_++;
						update_diag(frame_timer.elapsed());
						return frame_;
					}

					move_to_next_frame();

					seek_frame(in_file_, field2_pos, SEEK_SET);

					size_t field2_width;
					size_t field2_height;
					size_t field2_size = read_frame(in_file_, &field2_width, &field2_height, &field2);

					mmx_uint8_t* field_blend = new mmx_uint8_t[field1_size];

					float level = (1 - left_in_field1);
					blend_images(field1, field2, field_blend, field2_width, field2_height, 4, (uint8_t)(level * 63.0));

					delete last_field_;
					last_field_ = field2;
					last_field_size_ = field2_size;
					field1 = field_blend;

					left_in_field2 = modf(left_in_field1 + abs_speed_);
				}

				bool field2_is_field1 = false;

				if (left_in_field2 > 0.5f)
				{
					field2 = field1;
					left_of_last_field_ = left_in_field2 - 0.5f;
					field2_is_field1 = true;
				}
				else
				{
					long long field2_pos = read_index(in_idx_file_);

					if (field2_pos == -1)
					{	// There are no more frames
						result_framenum_++;
						update_diag(frame_timer.elapsed());
						return frame_;
					}

					move_to_next_frame();

					seek_frame(in_file_, field2_pos, SEEK_SET);

					size_t field2_width;
					size_t field2_height;
					size_t field2_size = read_frame(in_file_, &field2_width, &field2_height, &field2);

					mmx_uint8_t* field_blend = new mmx_uint8_t[field1_size];

					if (left_in_field2 > 0)
					{
						float level = (1 - left_in_field2);

						blend_images(last_field_, field2, field_blend, field2_width, field2_height, 4, (uint8_t)(level * 63.0));
					}
					else
					{
						memcpy(field_blend, field2, field2_size);
					}

					delete last_field_;
					last_field_ = field2;
					last_field_size_ = field2_size;
					field2 = field_blend;

					left_of_last_field_ = modf(left_in_field2 + abs_speed_);
				}

				mmx_uint8_t* full_frame = new mmx_uint8_t[last_field_size_ * 2];
				proper_interlace(field1, field2, full_frame);

				result_framenum_++;

				make_frame(full_frame, field1_size * 2, index_header_->width, index_header_->height, false);

				delete field1;
				if (!field2_is_field1)
					delete field2;
				delete full_frame;


				update_diag(frame_timer.elapsed());

				return frame_;
			}
		}
		else
		{
			if (last_field_ != NULL)
			{
				delete last_field_;
				last_field_ = NULL;
			}
		}


		// ELSE
		if (abs_speed_ >= 1.0f)
			sync_to_frame();

		long long field1_pos = read_index(in_idx_file_);

		if (field1_pos == -1)
		{	// There are no more frames
			result_framenum_++;
			update_diag(frame_timer.elapsed());
			return frame_;
		}

		move_to_next_frame();

		seek_frame(in_file_, field1_pos, SEEK_SET);

		mmx_uint8_t* field1;
		size_t field1_width;
		size_t field1_height;
		size_t field1_size = read_frame(in_file_, &field1_width, &field1_height, &field1);

		if ((!interlaced_) || (frame_divider_ > 1))
		{
			mmx_uint8_t* full_frame = NULL;

			if (interlaced_) 
			{
				full_frame = new mmx_uint8_t[field1_size * 2];
				field_double(field1, full_frame, index_header_->width, index_header_->height, 4);
			}
			else
			{
				full_frame = field1;
				field1 = NULL;
			}

			result_framenum_++;

			make_frame(full_frame, (interlaced_ ? field1_size * 2 : field1_size), index_header_->width, index_header_->height, (framenum_ % 2 != 0));

			if (field1 != NULL)
				delete field1;

			delete full_frame;

			update_diag(frame_timer.elapsed());

			return frame_;
		}

		long long field2_pos = read_index(in_idx_file_);

		move_to_next_frame();

		seek_frame(in_file_, field2_pos, SEEK_SET);

		mmx_uint8_t* field2;
		size_t field2_size = read_frame(in_file_, &field1_width, &field1_height, &field2);

		mmx_uint8_t* full_frame = new mmx_uint8_t[field1_size + field2_size];

		proper_interlace(field1, field2, full_frame);
		
		make_frame(full_frame, field1_size + field2_size, index_header_->width, index_header_->height, false);

		delete field1;
		delete field2;
		delete full_frame;

		update_diag(frame_timer.elapsed());

		return frame_;
	}
		
	virtual safe_ptr<core::basic_frame> last_frame() const override
	{
		return frame_;
	}

	virtual uint32_t nb_frames() const override
	{
		return 1000;
	}

	virtual std::wstring print() const override
	{
		return L"replay_producer[" + filename_ + L"|" + boost::lexical_cast<std::wstring>(interlaced_ ? framenum_ / 2 : framenum_)
			 + L"|" + boost::lexical_cast<std::wstring>(speed_)
			 + L"]";
	}

	virtual boost::property_tree::wptree info() const override
	{
		boost::property_tree::wptree info;
		info.add(L"type", L"replay-producer");
		info.add(L"filename", filename_);
		info.add(L"play-head", framenum_);
		info.add(L"speed", speed_);
		return info;
	}
};

safe_ptr<core::frame_producer> create_producer(const safe_ptr<core::frame_factory>& frame_factory, const std::vector<std::wstring>& params)
{
	static const std::vector<std::wstring> extensions = list_of(L"mav");
	std::wstring filename = env::media_folder() + L"\\" + params[0];
	
	auto ext = std::find_if(extensions.begin(), extensions.end(), [&](const std::wstring& ex) -> bool
		{					
			return boost::filesystem::is_regular_file(boost::filesystem::wpath(filename).replace_extension(ex));
		});

	if(ext == extensions.end())
		return core::frame_producer::empty();

	int sign = 0;
	long long start_frame = 0;
	if (params.size() >= 3) {
		if (params[1] == L"SEEK")
		{
			static const boost::wregex seek_exp(L"(?<SIGN>[|])?(?<VALUE>\\d+)", boost::regex::icase);
			boost::wsmatch what;
			if(boost::regex_match(params[2], what, seek_exp))
			{
				
				if(!what["SIGN"].str().empty())
				{
					if (what["SIGN"].str() == L"|")
						sign = -2;
					else
						sign = 0;
				}
				if(!what["VALUE"].str().empty())
				{
					start_frame = boost::lexical_cast<long long>(narrow(what["VALUE"].str()).c_str());
				}
			}
		}
	}

	return create_producer_print_proxy(
			make_safe<replay_producer>(frame_factory, filename + L"." + *ext, sign, start_frame));
}


}}