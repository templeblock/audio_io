#include <audio_io/audio_io.hpp>
#include <audio_io/private/audio_io.hpp>
#include <speex_resampler_cpp.hpp>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <utility>
#include <mutex>
#include <string.h>
#include <algorithm>
#include <thread>
#include <chrono>

namespace audio_io {
namespace implementation {

/**Code common to all backends, i.e. enumeration.*/

//these are the two steps in initialization, and are consequently put before the destructor.
void OutputDeviceImplementation::init(std::function<void(float*, int)> getBuffer, unsigned int inputBufferFrames, unsigned int inputBufferSr, unsigned int channels, unsigned int outputSr, unsigned int mixAhead) {
	input_buffer_frames = inputBufferFrames;
	mix_ahead = mixAhead;
	input_buffer_size = inputBufferFrames*channels;
	input_sr = inputBufferSr;
	output_sr = outputSr;
	this->channels = channels;
	buffer_statuses = new std::atomic<int>[mixAhead+1];
	get_buffer = getBuffer;
	for(unsigned int i = 0; i < mixAhead + 1; i++) buffer_statuses[i].store(0);
	if(input_sr != output_sr) is_resampling = true;
	output_buffer_frames = input_buffer_frames;
	if(output_sr != input_sr) {
		output_buffer_frames = (unsigned int)(output_buffer_frames*(double)output_sr)/input_sr;
	}
	output_buffer_size = output_buffer_frames*channels;
	buffers = new float*[mix_ahead+1];
	for(unsigned int i = 0; i < mix_ahead+1; i++) {
		buffers[i] = new float[output_buffer_size];
	}
}

void OutputDeviceImplementation::start() {
	mixing_thread_continue.test_and_set();
	mixing_thread = std::thread([this] () {mixingThreadFunction();});
	started = true;
}

OutputDeviceImplementation::~OutputDeviceImplementation() {
	stop();
	if(buffers != nullptr)
	for(unsigned int i = 0; i < mix_ahead+1; i++) {
		if(buffers[i]) delete[] buffers[i];
	}
	if(buffers) delete[] buffers;
	if(buffer_statuses) delete[] buffer_statuses;
}

void OutputDeviceImplementation::stop() {
	if(started == false) return;
	mixing_thread_continue.clear();
	mixing_thread.join();
	started=false;
}

void OutputDeviceImplementation::zeroOrNextBuffer(float* where) {
	if(buffer_statuses[next_output_buffer].load() == 1) {
		std::copy(buffers[next_output_buffer], buffers[next_output_buffer]+output_buffer_size, where);
		buffer_statuses[next_output_buffer].store(0);
		next_output_buffer += 1;
		next_output_buffer %= mix_ahead+1;
	}
	else {
		memset(where, 0, sizeof(float)*output_buffer_size);
	}
}

void OutputDeviceImplementation::mixingThreadFunction() {
	bool hasFilledQueueFirstTime = false;
	std::shared_ptr<speex_resampler_cpp::Resampler> resampler = speex_resampler_cpp::createResampler(input_buffer_frames, channels, input_sr, output_sr);
	unsigned int currentBuffer = 0;
	unsigned int sleepFor = (unsigned int)(((double)input_buffer_frames/input_sr)*1000);
	float* currentBlock = new float[input_buffer_size]();
	float* resampledBlock= new float[output_buffer_frames*channels]();
	while(mixing_thread_continue.test_and_set()) {
		if(buffer_statuses[currentBuffer].load()) { //we've done this one, but the callback hasn't gotten to it yet.
			if(sleepFor) std::this_thread::sleep_for(std::chrono::milliseconds(sleepFor));
			continue;
		}
		if(is_resampling == false) {
			get_buffer(currentBlock, channels);
		}
		else { //we need to resample.
			unsigned int got = 0;
			while(got < output_buffer_frames) {
				get_buffer(currentBlock, channels);
				resampler->read(currentBlock);
				got += resampler->write(resampledBlock+got, output_buffer_frames-got);
			}
		}
		if(is_resampling == false) {
			std::copy(currentBlock, currentBlock+output_buffer_size, buffers[currentBuffer]);
		}
		else {
			std::copy(resampledBlock, resampledBlock+output_buffer_size, buffers[currentBuffer]);
		}
		buffer_statuses[currentBuffer].store(1); //mark it as ready.
		currentBuffer ++;
		currentBuffer %= mix_ahead+1;
	}
}

OutputDeviceFactoryImplementation::~OutputDeviceFactoryImplementation() {
	for(auto p: created_devices) {
		auto strong = p.lock();
		if(strong) strong->stop();
	}
}

unsigned int OutputDeviceFactoryImplementation::getOutputCount() {
	return (unsigned int)output_count;
}

std::string OutputDeviceFactoryImplementation::getName() {
	return "Invalid backend: subclass failed to implement";
}

} //end namespace implementation
} //end namespace audio_io