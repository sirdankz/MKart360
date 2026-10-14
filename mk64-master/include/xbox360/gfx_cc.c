#include "gfx_cc.h"
#include <vector>
#include <string.h>
struct RegisteredCombiner {uint64_t combine;uint32_t options;CCFeatures features;};
static std::vector<RegisteredCombiner> registered;
extern "C" uint32_t gfx_cc_register(uint64_t combine,uint32_t options){
    for(size_t i=0;i<registered.size();++i)if(registered[i].combine==combine&&registered[i].options==options)return (uint32_t)i+1;
    RegisteredCombiner r;memset(&r,0,sizeof(r));r.combine=combine;r.options=options;
    CCFeatures &f=r.features;f.opt_alpha=(options&SHADER_OPT_ALPHA)!=0;f.opt_fog=(options&SHADER_OPT_FOG)!=0;f.opt_texture_edge=(options&SHADER_OPT_TEXTURE_EDGE)!=0;f.opt_noise=(options&SHADER_OPT_NOISE)!=0;f.opt_preserve_alpha=(options&SHADER_OPT_PRESERVE_ALPHA)!=0;f.two_cycle=(options&SHADER_OPT_2CYCLE)!=0;
    for(int cycle=0;cycle<(f.two_cycle?2:1);++cycle){
        // One-cycle RDP rendering uses the second set of mux fields.
        uint32_t word=(uint32_t)(combine>>((f.two_cycle?cycle:1)*32));
        uint8_t (*c)[4]=cycle?f.c2:f.c;
        for(int channel=0;channel<2;++channel){
            for(int j=0;j<4;++j)c[channel][j]=(uint8_t)((word>>(channel*16+j*4))&15);
            if(c[channel][0]==c[channel][1]||c[channel][2]==CC_0)c[channel][0]=c[channel][1]=c[channel][2]=CC_0;
            for(int j=0;j<4;++j){unsigned v=c[channel][j];
                int tex=(v==CC_TEXEL0||v==CC_TEXEL0A)?0:(v==CC_TEXEL1||v==CC_TEXEL1A)?1:-1;
                if(tex>=0){if(cycle==1)tex^=1;f.used_textures[tex]=true;}
                int input=(v==CC_PRIM||v==CC_PRIMA)?1:(v==CC_SHADE||v==CC_SHADEA)?2:(v==CC_ENV||v==CC_ENVA)?3:(v==CC_LOD||v==CC_PRIMLOD)?4:0;
                if(input>f.num_inputs)f.num_inputs=input;
            }
        }
    }
    registered.push_back(r);return (uint32_t)registered.size();
}
extern "C" void gfx_cc_get_features(uint32_t id,CCFeatures*f){memset(f,0,sizeof(*f));if(id&&id<=registered.size())*f=registered[id-1].features;}
