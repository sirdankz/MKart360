extern "C" void x360_log(const char*);
#include <xtl.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <vector>
#include <string.h>
#include <stdio.h>
#include <math.h>
extern "C" {
#include <PR/gbi.h>
#include "xbox360/gfx_rendering_api.h"
#include "xbox360/platform.h"
}
#include "xbox360_shader_source.h"
#include "xbox360/netplay.h"
#include "xbox360/netplay_view.h"
#include "xbox360/online_hud.h"
extern "C" { extern int gGamestate,gActiveScreenMode,gPlayerCountSelection1; }
static std::vector<float> local_view_vertices;
static bool local_view_logged=false;
extern "C" IDirect3DDevice9 *x360_d3d_device(void);
struct X360PackedVertexLayout {
    struct ShaderProgram *program;
    uint32_t floats_per_vertex;
    uint32_t geometry_mode;
    uint32_t last_vtx_w0, last_vtx_w1;
    uint32_t last_tri_w0, last_tri_w1;
    uint8_t num_inputs, use_texture, use_fog, use_alpha;
};
extern "C" struct X360PackedVertexLayout x360_gfx_batch_layout;
struct ShaderProgram {
    uint32_t shader_id;
    CCFeatures features;
    UINT stride;
    IDirect3DVertexShader9 *vs;
    IDirect3DPixelShader9 *ps;
    IDirect3DVertexDeclaration9 *decl;
};
static std::vector<ShaderProgram*> shaders;
static std::vector<IDirect3DTexture9*> textures;
static ShaderProgram *current;
static uint32_t selected[2];
static int upload_tile;
struct X360RectState { int x, y, width, height; };
static X360RectState active_viewport={0,0,1280,720};
static X360RectState active_scissor={0,0,1280,720};
static unsigned coordinate_range_log_count;
static unsigned abnormal_primitive_log_count;
static bool z01(void) { return true; }
static void unload_shader(ShaderProgram *p) { if(current==p)current=0; }
static void load_shader(ShaderProgram *p) {
    current=p; IDirect3DDevice9*d=x360_d3d_device();
    if(d && p) {d->SetVertexShader(p->vs);d->SetPixelShader(p->ps);d->SetVertexDeclaration(p->decl);}
}
static void add_element(D3DVERTEXELEMENT9 *e, unsigned &count, UINT &offset, DWORD type, BYTE usage, BYTE index, UINT bytes) {
    D3DVERTEXELEMENT9 v={0,(WORD)offset,type,D3DDECLMETHOD_DEFAULT,usage,index};
    e[count++]=v;offset+=bytes;
}
static ShaderProgram *create_shader(uint32_t id) {
    ShaderProgram*p=new ShaderProgram;memset(p,0,sizeof(*p));p->shader_id=id;
    gfx_cc_get_features(id,&p->features);
    CCFeatures &f=p->features;std::string source=x360_shader_source(f);
    IDirect3DDevice9*d=x360_d3d_device();
    for(int stage=0;stage<2;stage++) {
        ID3DXBuffer *code=0,*errors=0;
        HRESULT hr=D3DXCompileShader(source.c_str(),(UINT)source.size(),0,0,stage?"PS":"VS",stage?"ps_3_0":"vs_3_0",0,&code,&errors,0);
        if(errors){x360_log((const char*)errors->GetBufferPointer());errors->Release();}
        if(SUCCEEDED(hr) && code && d) {
            if(stage)hr=d->CreatePixelShader((DWORD*)code->GetBufferPointer(),&p->ps);
            else hr=d->CreateVertexShader((DWORD*)code->GetBufferPointer(),&p->vs);
        }
        if(code)code->Release();
        if(FAILED(hr))x360_log("MK64: combiner shader creation failed\n");
    }
    D3DVERTEXELEMENT9 e[9];unsigned n=0;UINT offset=0;
    add_element(e,n,offset,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_POSITION,0,16);
    if(f.used_textures[0])add_element(e,n,offset,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,0,8);
    if(f.used_textures[1])add_element(e,n,offset,D3DDECLTYPE_FLOAT2,D3DDECLUSAGE_TEXCOORD,1,8);
    if(f.opt_fog)add_element(e,n,offset,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TEXCOORD,2,16);
    for(int i=0;i<f.num_inputs;i++)add_element(e,n,offset,D3DDECLTYPE_FLOAT4,D3DDECLUSAGE_TEXCOORD,(BYTE)(i+3),16);
    const D3DVERTEXELEMENT9 end=D3DDECL_END();e[n]=end;p->stride=offset;
    if(d && FAILED(d->CreateVertexDeclaration(e,&p->decl)))x360_log("MK64: vertex declaration failed\n");
    shaders.push_back(p);load_shader(p);return p;
}
static ShaderProgram *lookup_shader(uint32_t id) {
    for(size_t i=0;i<shaders.size();i++)if(shaders[i]->shader_id==id)return shaders[i];return 0;
}
static void shader_info(ShaderProgram*p,uint8_t*n,bool tex[2]) {
    if(n)*n=p?(uint8_t)p->features.num_inputs:0;
    if(tex){tex[0]=p && p->features.used_textures[0];tex[1]=p && p->features.used_textures[1];}
}
static uint32_t new_tex(void){textures.push_back(0);return (uint32_t)textures.size();}

/* Reciclagem de texturas da GPU (texturas HD).
   Criar e destruir uma textura a cada envio custava ~1 ms por envio -- medido:
   ate ~560 ms por segundo numa corrida com sprites de kart em HD trocando de
   quadro o tempo todo. Agora a textura substituida vai para uma reserva, e um
   envio futuro do MESMO tamanho a reaproveita.
   Cuidado essencial: so reaproveitamos depois de X360_TEX_RECYCLE_DELAY quadros,
   para a GPU ja ter terminado de desenhar com ela. Reescrever uma textura ainda
   em uso foi o que causou as piscadas na primeira tentativa de reaproveitamento.
   A reserva tem limite de memoria; acima dele, as mais antigas sao liberadas. */
#define X360_TEX_RECYCLE_DELAY 3                     /* quadros */
#define X360_TEX_RECYCLE_MAX   512                   /* texturas na reserva */
#define X360_TEX_RECYCLE_BYTES (24u * 1024u * 1024u) /* memoria da reserva */
struct X360RecycledTex { IDirect3DTexture9 *t; int w, h; unsigned bytes, frame; };
static X360RecycledTex recycle_bin[X360_TEX_RECYCLE_MAX];
static int recycle_n;
static unsigned recycle_bytes, frame_no;

static void recycle_drop(int i) {
    recycle_bin[i].t->Release();
    recycle_bytes -= recycle_bin[i].bytes;
    recycle_bin[i] = recycle_bin[--recycle_n];
}
static int recycle_oldest(void) {
    int v = -1;
    for (int i = 0; i < recycle_n; i++)
        if (v < 0 || recycle_bin[i].frame < recycle_bin[v].frame) v = i;
    return v;
}
static IDirect3DTexture9 *recycle_take(int w, int h) {
    for (int i = 0; i < recycle_n; i++) {
        if (recycle_bin[i].w == w && recycle_bin[i].h == h &&
            frame_no - recycle_bin[i].frame >= X360_TEX_RECYCLE_DELAY) {
            IDirect3DTexture9 *t = recycle_bin[i].t;
            recycle_bytes -= recycle_bin[i].bytes;
            recycle_bin[i] = recycle_bin[--recycle_n];
            return t;
        }
    }
    return 0;
}
static void recycle_put(IDirect3DTexture9 *t) {
    D3DSURFACE_DESC desc;
    if (FAILED(t->GetLevelDesc(0, &desc))) { t->Release(); return; }
    const unsigned bytes = desc.Width * desc.Height * 4;
    if (bytes > X360_TEX_RECYCLE_BYTES) { t->Release(); return; }
    while (recycle_n > 0 && (recycle_n >= X360_TEX_RECYCLE_MAX ||
                             recycle_bytes + bytes > X360_TEX_RECYCLE_BYTES))
        recycle_drop(recycle_oldest());
    X360RecycledTex r = { t, (int)desc.Width, (int)desc.Height, bytes, frame_no };
    recycle_bin[recycle_n++] = r;
    recycle_bytes += bytes;
}
static void select_tex(int tile,uint32_t id) {
    if(tile<0||tile>1||!id||id>textures.size())return;
    selected[tile]=id;upload_tile=tile;
    IDirect3DDevice9*d=x360_d3d_device();if(d)d->SetTexture(tile,textures[id-1]);
}
static void upload_tex(const uint8_t*rgba,int w,int h) {
    IDirect3DDevice9*d=x360_d3d_device();uint32_t id=selected[upload_tile];
    if(!d||!rgba||w<=0||h<=0||w>4096||h>4096||!id||id>textures.size())return;
    IDirect3DTexture9*t=recycle_take(w,h);
    HRESULT hr=t?S_OK:d->CreateTexture(w,h,1,0,D3DFMT_LIN_A8R8G8B8,D3DPOOL_DEFAULT,&t,0);
    D3DLOCKED_RECT lock;
    if(SUCCEEDED(hr))hr=t->LockRect(0,&lock,0,0);
    if(FAILED(hr)){if(t)t->Release();x360_log("MK64: texture allocation/lock failed\n");return;}
    for(int y=0;y<h;y++) {
        DWORD *row=(DWORD*)((BYTE*)lock.pBits+y*lock.Pitch);
        for(int x=0;x<w;x++){const uint8_t*c=rgba+(y*w+x)*4;row[x]=((DWORD)c[3]<<24)|((DWORD)c[0]<<16)|((DWORD)c[1]<<8)|c[2];}
    }
    t->UnlockRect(0);
    IDirect3DTexture9*old=textures[id-1];textures[id-1]=t;
    for(int i=0;i<2;i++)if(selected[i]==id)d->SetTexture(i,t);
    if(old)recycle_put(old);
}
static DWORD address_mode(uint32_t mode){return (mode&2)?D3DTADDRESS_CLAMP:(mode&1)?D3DTADDRESS_MIRROR:D3DTADDRESS_WRAP;}
static void sampler(int s,bool linear,uint32_t cms,uint32_t cmt) {
    IDirect3DDevice9*d=x360_d3d_device();if(!d||s<0||s>1)return;
    d->SetSamplerState(s,D3DSAMP_MINFILTER,linear?D3DTEXF_LINEAR:D3DTEXF_POINT);
    d->SetSamplerState(s,D3DSAMP_MAGFILTER,linear?D3DTEXF_LINEAR:D3DTEXF_POINT);
    d->SetSamplerState(s,D3DSAMP_MIPFILTER,D3DTEXF_NONE);
    d->SetSamplerState(s,D3DSAMP_ADDRESSU,address_mode(cms));d->SetSamplerState(s,D3DSAMP_ADDRESSV,address_mode(cmt));
}
static void depth_test(bool e){IDirect3DDevice9*d=x360_d3d_device();if(d)d->SetRenderState(D3DRS_ZENABLE,e);}
static void depth_mask(bool e){IDirect3DDevice9*d=x360_d3d_device();if(d)d->SetRenderState(D3DRS_ZWRITEENABLE,e);}
static void zmode(bool e){IDirect3DDevice9*d=x360_d3d_device();if(d){float bias=e?-0.00001f:0;DWORD bits;memcpy(&bits,&bias,4);d->SetRenderState(D3DRS_DEPTHBIAS,bits);}}
static int clamp_coordinate(int value,int low,int high){return value<low?low:(value>high?high:value);}
/* X360_CRT_480I_NATIVE_BACKBUFFER:
 * MK64/gfx_pc continues to think in 1280x720. Convert only the final D3D
 * rectangles to the real Xbox framebuffer so the 720p path stays identical. */
static mkview::Rect x360_framebuffer_rect(mkview::Rect logical){
    const int sw=(int)x360_video_width(),sh=(int)x360_video_height();
    int left=(logical.x*sw+640)/1280;
    int right=((logical.x+logical.w)*sw+640)/1280;
    int top=(logical.y*sh+360)/720;
    int bottom=((logical.y+logical.h)*sh+360)/720;
    left=clamp_coordinate(left,0,sw);right=clamp_coordinate(right,0,sw);
    top=clamp_coordinate(top,0,sh);bottom=clamp_coordinate(bottom,0,sh);
    if(logical.w>0&&right<=left&&left<sw)right=left+1;
    if(logical.h>0&&bottom<=top&&top<sh)bottom=top+1;
    mkview::Rect r={left,top,right-left,bottom-top};return r;
}
static void viewport(int x,int y,int w,int h){
    IDirect3DDevice9*d=x360_d3d_device();if(!d)return;
    const int screen_w=1280,screen_h=720;
    int left=clamp_coordinate(x,0,screen_w),right=clamp_coordinate(x+w,0,screen_w);
    int top=clamp_coordinate(screen_h-y-h,0,screen_h),bottom=clamp_coordinate(screen_h-y,0,screen_h);
    if(right<=left||bottom<=top){
        if(abnormal_primitive_log_count<8){char message[192];_snprintf(message,sizeof(message)-1,
            "MK64: abnormal viewport bottomleft=%d,%d %dx%d screen=%dx%d\n",x,y,w,h,screen_w,screen_h);
            message[sizeof(message)-1]=0;x360_log(message);++abnormal_primitive_log_count;}return;
    }
    active_viewport.x=left;active_viewport.y=top;active_viewport.width=right-left;active_viewport.height=bottom-top;
    mkview::Rect logical={left,top,right-left,bottom-top};
    mkview::Rect physical=x360_framebuffer_rect(logical);
    D3DVIEWPORT9 v={(DWORD)physical.x,(DWORD)physical.y,(DWORD)physical.w,(DWORD)physical.h,0,1};d->SetViewport(&v);
}
static void scissor(int x,int y,int w,int h){
    IDirect3DDevice9*d=x360_d3d_device();if(!d)return;
    const int screen_w=1280,screen_h=720;
    int left=clamp_coordinate(x,0,screen_w),right=clamp_coordinate(x+w,0,screen_w);
    int top=clamp_coordinate(screen_h-y-h,0,screen_h),bottom=clamp_coordinate(screen_h-y,0,screen_h);
    if(right<left)right=left;if(bottom<top)bottom=top;
    active_scissor.x=left;active_scissor.y=top;active_scissor.width=right-left;active_scissor.height=bottom-top;
    mkview::Rect logical={left,top,right-left,bottom-top};
    mkview::Rect physical=x360_framebuffer_rect(logical);
    RECT r={physical.x,physical.y,physical.x+physical.w,physical.y+physical.h};d->SetScissorRect(&r);
}
static void use_alpha(bool e){IDirect3DDevice9*d=x360_d3d_device();if(d){d->SetRenderState(D3DRS_ALPHABLENDENABLE,e);d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);}}
static void draw(float*buf,size_t len,size_t tris){
    IDirect3DDevice9*d=x360_d3d_device();ShaderProgram *batch=x360_gfx_batch_layout.program;
    if(!d||!batch||!buf||!tris)return;
    const size_t vertices=tris*3;
    const size_t actual_floats=vertices ? len/vertices : 0;
    const UINT packed_stride=x360_gfx_batch_layout.floats_per_vertex*sizeof(float);
    const bool divisible=vertices && (len%vertices)==0;
    const bool matches=divisible && actual_floats==x360_gfx_batch_layout.floats_per_vertex && packed_stride==batch->stride;
    if(!matches){
        static uint32_t signatures[8];static unsigned signature_count;
        uint32_t sig=(uint32_t)actual_floats|(batch->stride<<8)|(batch->shader_id*33);
        bool seen=false;for(unsigned i=0;i<signature_count;i++)if(signatures[i]==sig)seen=true;
        if(!seen && signature_count<8){
            signatures[signature_count++]=sig;char message[384];
            _snprintf(message,sizeof(message)-1,
                "MK64: vertex layout mismatch actual=%uB expected=%uB packed=%uB floats=%u tris=%u shader=%08X type=%s inputs=%u tex=%u fog=%u alpha=%u geom=%08X vtx=%08X/%08X tri=%08X/%08X\n",
                (unsigned)(actual_floats*sizeof(float)),(unsigned)batch->stride,(unsigned)packed_stride,
                (unsigned)len,(unsigned)tris,(unsigned)batch->shader_id,
                (x360_gfx_batch_layout.geometry_mode&G_LIGHTING)?"Vtx_tn":"Vtx_t",
                x360_gfx_batch_layout.num_inputs,x360_gfx_batch_layout.use_texture,
                x360_gfx_batch_layout.use_fog,x360_gfx_batch_layout.use_alpha,
                (unsigned)x360_gfx_batch_layout.geometry_mode,
                (unsigned)x360_gfx_batch_layout.last_vtx_w0,(unsigned)x360_gfx_batch_layout.last_vtx_w1,
                (unsigned)x360_gfx_batch_layout.last_tri_w0,(unsigned)x360_gfx_batch_layout.last_tri_w1);
            message[sizeof(message)-1]=0;x360_log(message);
        }
        return;
    }
    mkview::Rect crop,visible;
    mkview::Rect vp={active_viewport.x,active_viewport.y,active_viewport.width,active_viewport.height};
    mkview::Rect sc={active_scissor.x,active_scissor.y,active_scissor.width,active_scissor.height};
    const int online_players=x360_net_player_count();
    const int local_slot=x360_net_local_slot();
    const bool local=!x360_gfx_online_hud && x360_net_active() && !x360_net8_active() && gGamestate==4 &&
        online_players>=2 && online_players<=4 &&
        gPlayerCountSelection1==online_players &&
        mkview::crop(gActiveScreenMode,online_players,local_slot,crop);

    if(local && (!mkview::intersect(sc,vp,visible)||!mkview::intersect(visible,crop,visible)))return;
    /* Diagnostic scans are unnecessary once their bounded samples are logged. */
    if(x360_logging_enabled() && (coordinate_range_log_count<6 || abnormal_primitive_log_count<8)) {
    float clip_min[4]={1.0e30f,1.0e30f,1.0e30f,1.0e30f};
    float clip_max[4]={-1.0e30f,-1.0e30f,-1.0e30f,-1.0e30f};
    float ndc_min[3]={1.0e30f,1.0e30f,1.0e30f};
    float ndc_max[3]={-1.0e30f,-1.0e30f,-1.0e30f};
    bool bad_number=false;
    for(size_t vertex=0;vertex<vertices;vertex++){
        const float *p=buf+vertex*x360_gfx_batch_layout.floats_per_vertex;
        for(int axis=0;axis<4;axis++){float value=p[axis];if(value!=value||fabsf(value)>1.0e30f)bad_number=true;
            if(value<clip_min[axis])clip_min[axis]=value;if(value>clip_max[axis])clip_max[axis]=value;}
        if(fabsf(p[3])<1.0e-8f){bad_number=true;continue;}
        for(int axis=0;axis<3;axis++){float value=p[axis]/p[3];
            if(value<ndc_min[axis])ndc_min[axis]=value;if(value>ndc_max[axis])ndc_max[axis]=value;}
    }
    if(coordinate_range_log_count<6){char message[448];_snprintf(message,sizeof(message)-1,
        "MK64: batch-coords[%u] tris=%u shader=%08X clip=[%g..%g,%g..%g,%g..%g,%g..%g] "
        "ndc=[%g..%g,%g..%g,%g..%g] viewport=%d,%d %dx%d scissor=%d,%d %dx%d\n",
        coordinate_range_log_count,(unsigned)tris,(unsigned)batch->shader_id,
        clip_min[0],clip_max[0],clip_min[1],clip_max[1],clip_min[2],clip_max[2],clip_min[3],clip_max[3],
        ndc_min[0],ndc_max[0],ndc_min[1],ndc_max[1],ndc_min[2],ndc_max[2],
        active_viewport.x,active_viewport.y,active_viewport.width,active_viewport.height,
        active_scissor.x,active_scissor.y,active_scissor.width,active_scissor.height);
        message[sizeof(message)-1]=0;x360_log(message);++coordinate_range_log_count;}
    for(size_t tri=0;tri<tris&&abnormal_primitive_log_count<8;tri++){
        float min_x=1.0e30f,max_x=-1.0e30f,min_y=1.0e30f,max_y=-1.0e30f;bool abnormal=bad_number;
        for(int corner=0;corner<3;corner++){const float*p=buf+(tri*3+corner)*x360_gfx_batch_layout.floats_per_vertex;
            if(fabsf(p[3])<1.0e-8f){abnormal=true;continue;}float nx=p[0]/p[3],ny=p[1]/p[3];
            if(nx<min_x)min_x=nx;if(nx>max_x)max_x=nx;if(ny<min_y)min_y=ny;if(ny>max_y)max_y=ny;
            if(nx!=nx||ny!=ny||fabsf(nx)>1.05f||fabsf(ny)>1.05f)abnormal=true;}
        if((max_x-min_x)>2.05f||(max_y-min_y)>2.05f)abnormal=true;
        if(abnormal){const float*a=buf+(tri*3)*x360_gfx_batch_layout.floats_per_vertex;
            const float*b=a+x360_gfx_batch_layout.floats_per_vertex;const float*c=b+x360_gfx_batch_layout.floats_per_vertex;
            char message[384];_snprintf(message,sizeof(message)-1,
              "MK64: abnormal primitive[%u] tri=%u shader=%08X ndc=%g..%g,%g..%g clip0=(%g,%g,%g,%g) clip1=(%g,%g,%g,%g) clip2=(%g,%g,%g,%g)\n",
              abnormal_primitive_log_count,(unsigned)tri,(unsigned)batch->shader_id,min_x,max_x,min_y,max_y,
              a[0],a[1],a[2],a[3],b[0],b[1],b[2],b[3],c[0],c[1],c[2],c[3]);
            message[sizeof(message)-1]=0;x360_log(message);++abnormal_primitive_log_count;}
    }
    }
    /* A flush can be caused by the state change which selects the next shader.
     * Rebind the program captured when this batch was packed. */
    ShaderProgram *selected_program=current;
    load_shader(batch);
    if(!batch->vs||!batch->ps||!batch->decl){if(selected_program!=batch)load_shader(selected_program);return;}

    /* MK64 PER-CONSOLE FULLSCREEN VIEW
     * MK64 still renders native 2P/3P/4P split-screen internally.
     * Online racing presentation keeps only this console's assigned slot and
     * expands that viewport to the full 1280x720 Xbox output.
     */
    const bool x360_legacy_wide = x360_display_aspect() > 1.5f;
    const bool x360_physical_wide = x360_video_widescreen()!=0;
    const bool x360_exact_legacy_hd_wide =
        x360_legacy_wide && x360_physical_wide &&
        x360_video_width()==1280 && x360_video_height()==720;
    /* X360_ONLINE_MATCHED_ASPECT_FULLSCREEN_V1
     * When the selected game aspect already matches the physical display
     * (notably 4:3 on a 480i CRT), the online local view owns the complete
     * physical framebuffer. */
    const bool x360_matching_output_aspect =
        x360_legacy_wide == x360_physical_wide;
    const mkview::Rect out = mkview::output(x360_legacy_wide,x360_physical_wide);
    if(local){
        if(!local_view_logged){
            char message[192];
            _snprintf(message,sizeof(message)-1,
                "MK64VIEW: per-console fullscreen active slot=P%d players=%d mode=%d crop=%d,%d %dx%d\n",
                local_slot+1,online_players,gActiveScreenMode,crop.x,crop.y,crop.w,crop.h);
            message[sizeof(message)-1]=0;x360_log(message);local_view_logged=true;
        }
        if(!mkview::intersect(sc,vp,visible)||!mkview::intersect(visible,crop,visible)){
            if(selected_program!=batch)load_shader(selected_program);return;
        }
        if (x360_exact_legacy_hd_wide) {
            /* Exact known-good B23/B23F 16:9 local fullscreen presentation. */
            D3DVIEWPORT9 full={0,0,1280,720,0,1};
            d->SetViewport(&full);
            RECT clip={
                (visible.x-crop.x)*1280/crop.w,
                (visible.y-crop.y)*720/crop.h,
                (visible.x+visible.w-crop.x)*1280/crop.w,
                (visible.y+visible.h-crop.y)*720/crop.h
            };
            d->SetScissorRect(&clip);
        } else if (x360_matching_output_aspect) {
            /* Matching aspect: this console's selected online view is truly
             * fullscreen.  Do not preserve a split-screen-derived scissor. */
            const DWORD sw=(DWORD)x360_video_width();
            const DWORD sh=(DWORD)x360_video_height();
            D3DVIEWPORT9 full={0,0,sw,sh,0,1};
            d->SetViewport(&full);
            RECT clip={0,0,(LONG)sw,(LONG)sh};
            d->SetScissorRect(&clip);
        } else {
            /* Mismatched aspect only: retain pillarbox/letterbox mapping. */
            mkview::Rect fbout=x360_framebuffer_rect(out);
            D3DVIEWPORT9 full={
                (DWORD)fbout.x,(DWORD)fbout.y,(DWORD)fbout.w,(DWORD)fbout.h,0,1
            };
            d->SetViewport(&full);
            mkview::Rect logical_clip={
                out.x+(visible.x-crop.x)*out.w/crop.w,
                out.y+(visible.y-crop.y)*out.h/crop.h,
                (visible.w*out.w)/crop.w,
                (visible.h*out.h)/crop.h
            };
            mkview::Rect fbclip=x360_framebuffer_rect(logical_clip);
            RECT clip={
                fbclip.x,fbclip.y,fbclip.x+fbclip.w,fbclip.y+fbclip.h
            };
            d->SetScissorRect(&clip);
        }
        local_view_vertices.assign(buf,buf+len);
        for(size_t v=0;v<vertices;++v)
            mkview::vertex(&local_view_vertices[v*x360_gfx_batch_layout.floats_per_vertex],vp,crop);

        /* Projection and rectangle sizing are corrected before clipping.
         * Never infer quads from six consecutive, potentially clipped vertices. */
        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,(UINT)tris,&local_view_vertices[0],packed_stride);
    }else{
        if (x360_exact_legacy_hd_wide) {
            /*
             * Exact legacy 16:9 path from before the display-mode option:
             * no output_rect(), no second presentation transform.
             */
            D3DVIEWPORT9 original={
                (DWORD)vp.x,(DWORD)vp.y,(DWORD)vp.w,(DWORD)vp.h,0,1
            };
            d->SetViewport(&original);
            RECT clip={sc.x,sc.y,sc.x+sc.w,sc.y+sc.h};
            d->SetScissorRect(&clip);
        } else {
            /* General aspect path. Logical viewport/scissor stay 1280x720;
             * only their final D3D rectangles become framebuffer coordinates. */
            mkview::Rect mapped_vp=mkview::output_rect(vp,out);
            mkview::Rect mapped_sc=mkview::output_rect(sc,out);
            mkview::Rect fbvp=x360_framebuffer_rect(mapped_vp);
            mkview::Rect fbsc=x360_framebuffer_rect(mapped_sc);
            D3DVIEWPORT9 original={
                (DWORD)fbvp.x,(DWORD)fbvp.y,
                (DWORD)fbvp.w,(DWORD)fbvp.h,0,1
            };
            d->SetViewport(&original);
            RECT clip={
                fbsc.x,fbsc.y,
                fbsc.x+fbsc.w,fbsc.y+fbsc.h
            };
            d->SetScissorRect(&clip);
        }

        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST,(UINT)tris,buf,packed_stride);
        if(!x360_net_active())local_view_logged=false;
    }
    if(selected_program!=batch)load_shader(selected_program);
}
static void init(void){
    current=0;selected[0]=selected[1]=0;upload_tile=0;
    IDirect3DDevice9*d=x360_d3d_device();if(!d)return;
    d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);d->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE,TRUE);
}
static void resize(void){}
static void start_frame(void){
    IDirect3DDevice9*d=x360_d3d_device();if(d){
        const DWORD sw=(DWORD)x360_video_width(),sh=(DWORD)x360_video_height();
        D3DVIEWPORT9 full={0,0,sw,sh,0,1};d->SetViewport(&full);
        RECT all={0,0,(LONG)sw,(LONG)sh};d->SetScissorRect(&all);
        d->Clear(0,0,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xff000000,1,0);d->BeginScene();
    }
}
extern "C" void x360_net8_draw_hud(void);
static void end_frame(void){IDirect3DDevice9*d=x360_d3d_device();if(d){x360_net8_draw_hud();d->EndScene();}++frame_no;}
static void finish(void){}
extern "C" struct GfxRenderingAPI gfx_xbox360_api={z01,unload_shader,load_shader,create_shader,lookup_shader,shader_info,new_tex,select_tex,upload_tex,sampler,depth_test,depth_mask,zmode,viewport,scissor,use_alpha,draw,init,resize,start_frame,end_frame,finish};
