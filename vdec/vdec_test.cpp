extern "C" {
#include <libavcodec/avcodec.h>
}
#include <iostream>
#include <vector>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <memory>
#include "vdec_ffmpeg.hpp"

// Mock Renderer
class MockRenderer : public Renderer {
public:
    MockRenderer() : Renderer(Disable, nullptr, nullptr, nullptr, nullptr, false) {}
    bool render_frame(DecodedUnit* du) override {
        printf("MOCK_RENDER: Frame rendered! PTS=%ld, W=%d, H=%d\n", du->pts, du->width, du->height);
        return true;
    }
    void queue_frame(std::shared_ptr<DecodedUnit> du) override {
        render_frame(du.get());
    }
    void update_osd() {}
    void init() {}
    void cleanup() {}
};

int main(int argc, char* argv[]) {
    // setbuf(stdout, NULL);
    if (argc < 2) {
        printf("Usage: %s <hevc_file> [decoder]\n", argv[0]);
        return 1;
    }

    std::string filename = argv[1];
    std::string decoder_name = "vaapi"; // Default
    if (argc >= 3) {
        decoder_name = argv[2];
    }
    
    // Read file
    std::ifstream file(filename, std::ios::binary);
    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (buffer.empty()) {
        printf("Failed to read file: %s\n", filename.c_str());
        return 1;
    }

    volatile bool should_stop = false;
    auto renderer = std::make_shared<MockRenderer>();
    VdecFfmpeg vdec(VideoCodec::H265, decoder_name.c_str(), renderer, &should_stop);

    // Init Parser
    AVCodecParserContext* parser = av_parser_init(AV_CODEC_ID_HEVC);
    AVCodecContext* ctx = avcodec_alloc_context3(NULL);
    if (!parser || !ctx) {
        printf("Failed to init parser\n");
        return 1;
    }

    printf("Feeding bitstream from %s using AVParser...\n", filename.c_str());

    uint8_t* data = buffer.data();
    int data_size = buffer.size();
    int64_t pts = 0;
    
    while (data_size > 0) {
        AVPacket* pkt = av_packet_alloc();
        int ret = av_parser_parse2(parser, ctx, &pkt->data, &pkt->size,
                               data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (ret < 0) {
            printf("Error in parser\n");
            av_packet_free(&pkt);
            break;
        }
        
        data += ret;
        data_size -= ret;

        if (pkt->size > 0) {
             // Determine First NAL Type for feed_packet_to_decoder
             int nal_type = 0;
             uint8_t* p = pkt->data;
             int type_off = -1;
             
             // Simple scan for first start code
             for(int i=0; i< std::min(pkt->size, 100); i++) {
                 if (i+3 < pkt->size && p[i]==0 && p[i+1]==0 && p[i+2]==1) { type_off = i+3; break; }
                 if (i+4 < pkt->size && p[i]==0 && p[i+1]==0 && p[i+2]==0 && p[i+3]==1) { type_off = i+4; break; }
             }
             
             if (type_off != -1) {
                 nal_type = (p[type_off] >> 1) & 0x3F;
             }
             
             // Bruteforce removed
             // pkt->flags |= AV_PKT_FLAG_KEY;
             
             printf("VDEC_TEST: Parsed Packet size %d, NAL Type %d\n", pkt->size, nal_type);
             
             // Feed it
             vdec.feed_packet_to_decoder(pkt->data, pkt->size, pts, pts, nal_type, 0, 0);
             pts += 3000;
             usleep(5000);
        }
        av_packet_free(&pkt);
    }

    printf("VDEC_TEST: Finished feeding. Flushing...\n");
    vdec.feed_packet_to_decoder(NULL, 0, pts, pts, 0, 0, 0); 
    sleep(1);
    
    av_parser_close(parser);
    avcodec_free_context(&ctx);
    return 0;
}
