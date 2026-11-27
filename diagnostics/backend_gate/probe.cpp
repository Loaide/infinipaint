// Isolated diagnostic APK: capabilities of the actual loader in this process.
// No owner document access, frame benchmark, driver override or GPU waits.
#include <android/native_activity.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <vulkan/vulkan.h>
#include <cstring>
#include <vector>
#define LOG(...) __android_log_print(ANDROID_LOG_INFO,"IP_BACKEND",__VA_ARGS__)

static void names(const char* kind,const char* list) {
    // One capability per line: avoid logcat's maximum record length.
    if (!list) {LOG("%s unavailable",kind);return;}
    while (*list) {
        list+=std::strspn(list," ");
        auto n=std::strcspn(list," ");
        if(n)LOG("%s %.*s",kind,int(n),list);
        list+=n;
    }
}
static void glProbe(EGLDisplay d,int major,int minor) {
    EGLint configAttrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,
        major==2?EGL_OPENGL_ES2_BIT:EGL_OPENGL_ES3_BIT_KHR,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,
        EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_SAMPLES,0,EGL_NONE};
    EGLConfig c{}; EGLint n=0;
    if(!eglChooseConfig(d,configAttrs,&c,1,&n)||!n){LOG("no config request=%d.%d",major,minor);return;}
    EGLint size[]={EGL_WIDTH,8,EGL_HEIGHT,8,EGL_NONE};
    EGLSurface s=eglCreatePbufferSurface(d,c,size);
    EGLint vers[]={EGL_CONTEXT_CLIENT_VERSION,major,EGL_CONTEXT_MINOR_VERSION_KHR,minor,EGL_NONE};
    if(!minor)vers[2]=EGL_NONE;
    EGLContext ctx=eglCreateContext(d,c,EGL_NO_CONTEXT,vers);
    if(ctx!=EGL_NO_CONTEXT && s!=EGL_NO_SURFACE && eglMakeCurrent(d,s,s,ctx)){
        LOG("request=%d.%d actual=%s renderer=%s vendor=%s GLSL=%s",major,minor,
            glGetString(GL_VERSION),glGetString(GL_RENDERER),glGetString(GL_VENDOR),
            glGetString(GL_SHADING_LANGUAGE_VERSION));
        names("GL",reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)));
        for(GLenum stage:{GL_VERTEX_SHADER,GL_FRAGMENT_SHADER})for(GLenum type:{GL_MEDIUM_FLOAT,GL_HIGH_FLOAT}){
            GLint r[2]{},p=0;glGetShaderPrecisionFormat(stage,type,r,&p);
            LOG("precision stage=%x type=%x range=%d:%d bits=%d",stage,type,r[0],r[1],p);
        }
        if(major==3 && minor>=1){
            GLint inv=0,shared=0;
            glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS,&inv);
            glGetIntegerv(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE,&shared);
            auto e=glGetError();
            LOG("compute invocations=%d sharedBytes=%d glError=%x",inv,shared,e);
        }
        LOG("timer entrypoints queryCounter/availability/ui64=%d/%d/%d",
            !!eglGetProcAddress("glQueryCounterEXT"),!!eglGetProcAddress("glGetQueryObjectuivEXT"),
            !!eglGetProcAddress("glGetQueryObjectui64vEXT"));
        eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    }else LOG("request=%d.%d unavailable EGL error=%x",major,minor,eglGetError());
    if(ctx!=EGL_NO_CONTEXT)eglDestroyContext(d,ctx);
    if(s!=EGL_NO_SURFACE)eglDestroySurface(d,s);
}
static void vkProbe(){
    uint32_t loader=VK_API_VERSION_1_0;
    auto version=reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr,"vkEnumerateInstanceVersion"));
    if(version && version(&loader)!=VK_SUCCESS){LOG("VK loader version unavailable");return;}
    LOG("VK loader=%u.%u.%u",VK_VERSION_MAJOR(loader),VK_VERSION_MINOR(loader),VK_VERSION_PATCH(loader));
    VkInstanceCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    VkInstance instance{};auto result=vkCreateInstance(&ci,nullptr,&instance);
    if(result!=VK_SUCCESS){LOG("VK instance=%d",result);return;}
    uint32_t count=0;
    if(vkEnumeratePhysicalDevices(instance,&count,nullptr)==VK_SUCCESS && count && count<=16){
        std::vector<VkPhysicalDevice> ds(count);
        if(vkEnumeratePhysicalDevices(instance,&count,ds.data())==VK_SUCCESS)
        for(uint32_t i=0;i<count;++i){
            auto d=ds[i];VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(d,&p);
            LOG("VK device=%s vendor=%x id=%x type=%d api=%u.%u.%u driver=%u timestampPeriod_ns=%g computeInvocations=%u sharedBytes=%u",
                p.deviceName,p.vendorID,p.deviceID,p.deviceType,VK_VERSION_MAJOR(p.apiVersion),
                VK_VERSION_MINOR(p.apiVersion),VK_VERSION_PATCH(p.apiVersion),p.driverVersion,
                p.limits.timestampPeriod,p.limits.maxComputeWorkGroupInvocations,p.limits.maxComputeSharedMemorySize);
            VkPhysicalDeviceFeatures f{};vkGetPhysicalDeviceFeatures(d,&f);
            VkFormatProperties fmt{};vkGetPhysicalDeviceFormatProperties(d,VK_FORMAT_R8G8B8A8_UNORM,&fmt);
            LOG("VK rgba8 optimal=%x independentBlend=%u float64=%u timestampComputeAndGraphics=%u",
                fmt.optimalTilingFeatures,f.independentBlend,f.shaderFloat64,p.limits.timestampComputeAndGraphics);
            uint32_t qn=0;vkGetPhysicalDeviceQueueFamilyProperties(d,&qn,nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);vkGetPhysicalDeviceQueueFamilyProperties(d,&qn,qs.data());
            for(uint32_t j=0;j<qn;j++)LOG("VK queue=%u flags=%x count=%u timestampBits=%u",j,qs[j].queueFlags,qs[j].queueCount,qs[j].timestampValidBits);
            uint32_t en=0;
            if(vkEnumerateDeviceExtensionProperties(d,nullptr,&en,nullptr)==VK_SUCCESS && en<=4096){
                std::vector<VkExtensionProperties> ext(en);
                if(vkEnumerateDeviceExtensionProperties(d,nullptr,&en,ext.data())==VK_SUCCESS)
                    for(uint32_t j=0;j<en;j++)LOG("VK_EXT %s",ext[j].extensionName);
            }
        }
    }else LOG("VK enumeration unavailable or no device count=%u",count);
    vkDestroyInstance(instance,nullptr);
}
extern "C" void ANativeActivity_onCreate(ANativeActivity* a,void*,size_t){
    LOG("begin inventory v1");
    EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if(eglInitialize(d,nullptr,nullptr)){
        LOG("EGL vendor=%s version=%s",eglQueryString(d,EGL_VENDOR),eglQueryString(d,EGL_VERSION));
        names("EGL",eglQueryString(d,EGL_EXTENSIONS));
        glProbe(d,2,0);glProbe(d,3,0);glProbe(d,3,1);glProbe(d,3,2);
        eglTerminate(d);
    }else LOG("EGL unavailable error=%x",eglGetError());
    vkProbe();LOG("end inventory v1");ANativeActivity_finish(a);
}
