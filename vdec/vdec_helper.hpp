#ifndef VDEC_HELPER_H  // Check if VDEC_HELPER_H is not defined
#define VDEC_HELPER_H  // Define VDEC_HELPER_H

#include <memory>

#include "vdec.hpp"
#ifdef USE_RKMPP
#include "vdec_rk.hpp"
#endif
#include "vdec_ffmpeg.hpp"
#include "../renderer.hpp"


std::shared_ptr<Vdec> new_vdec(VideoCodec codec, std::string decoder_name, std::shared_ptr<Renderer> renderer, std::shared_ptr<DrmDevice> dev, volatile bool* signal_stop) {
    if(decoder_name.empty()) {
        // Default to ffmpeg
        // return nullptr; 
    }
    #ifdef USE_RKMPP
    if(decoder_name == "rkmpp") {
        return std::make_shared<VdecRK>(codec, renderer, dev, signal_stop);
    }
    #endif
    return std::make_shared<VdecFfmpeg>(codec, decoder_name, renderer, signal_stop);
}



#endif