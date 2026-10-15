#include "MetalioAudioFilePlayer.h"
#if FREEINK_DEVICE_METALIO_EINK4
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <HalStorage.h>
#include <Logging.h>
#include "MetalioAudioCodec.h"
namespace metalio_audio_file_player { namespace {
constexpr char kTag[]="METALIO-FILE";
constexpr std::size_t kBufferBytes=32*1024,kBufferCount=2;
constexpr uint32_t kTaskStackBytes=12*1024;
constexpr UBaseType_t kReaderPriority=3,kOutputPriority=4;
constexpr UBaseType_t kTaskCaps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
struct WavInfo { uint16_t format=0,channels=0,bitsPerSample=0; uint32_t sampleRate=0; std::size_t dataOffset=0,dataBytes=0; };
uint16_t le16(const uint8_t*p){return uint16_t(p[0]|(uint16_t(p[1])<<8));}
uint32_t le32(const uint8_t*p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
bool readExact(HalFile&f,void*d,std::size_t n){auto*p=static_cast<uint8_t*>(d);std::size_t done=0;while(done<n){int r=f.read(p+done,n-done);if(r<=0)return false;done+=std::size_t(r);}return true;}
bool skip(HalFile&f,std::size_t n){auto p=f.position(),s=f.fileSize();return p<=s&&n<=s-p&&f.seek(p+n);}
bool parseWav(HalFile&f,WavInfo&i){uint8_t h[12];if(!f.seek(0)||!readExact(f,h,12)||std::memcmp(h,"RIFF",4)||std::memcmp(h+8,"WAVE",4))return false;auto s=f.fileSize();bool fmt=false,data=false;while(f.position()+8<=s){uint8_t ch[8];if(!readExact(f,ch,8))return false;const uint32_t n = le32(ch + 4);
const std::size_t p = f.position();if(p>s||std::size_t(n)>s-p)return false;if(!std::memcmp(ch,"fmt ",4)){if(n<16)return false;uint8_t x[16];if(!readExact(f,x,16))return false;i.format=le16(x);i.channels=le16(x+2);i.sampleRate=le32(x+4);i.bitsPerSample=le16(x+14);fmt=true;if(n>16&&!skip(f,n-16))return false;}else if(!std::memcmp(ch,"data",4)){i.dataOffset=p;i.dataBytes=n;data=true;if(fmt)break;if(!skip(f,n))return false;}else if(!skip(f,n))return false;if(n&1u&&!skip(f,1))return false;}return fmt&&data&&i.format==1&&i.channels==1&&i.sampleRate==16000&&i.bitsPerSample==16&&(i.dataBytes%2)==0;}
struct Block {uint8_t*data=nullptr;std::size_t samples=0;bool end=false;};
}}
using namespace metalio_audio_file_player;
namespace metalio_audio_file_player {
struct Player::Impl {
  std::mutex mutex; std::condition_variable cv;
  TaskHandle_t readerTask=nullptr,outputTask=nullptr;
  QueueHandle_t freeQ=nullptr,readyQ=nullptr;
  uint8_t*buf[kBufferCount]={nullptr,nullptr};
  bool stopRequested=false,running=false,success=false,failed=false;
  char error[128]=""; std::string path; MetalioAudioCodec codec;
  void err(const char*s){std::lock_guard<std::mutex>l(mutex);std::strncpy(error,s?s:"Unknown error",sizeof(error)-1);error[sizeof(error)-1]=0;}
  bool stopped(){std::lock_guard<std::mutex>l(mutex);return stopRequested;}
  static void rd(void*p){auto*self=static_cast<Impl*>(p);self->reader();{std::lock_guard<std::mutex>l(self->mutex);self->readerTask=nullptr;}self->cv.notify_all();vTaskDeleteWithCaps(nullptr);}
  static void out(void*p){auto*self=static_cast<Impl*>(p);self->output();{std::lock_guard<std::mutex>l(self->mutex);self->outputTask=nullptr;}self->cv.notify_all();vTaskDeleteWithCaps(nullptr);}
  void finish(bool ok){ {std::lock_guard<std::mutex>l(mutex);success=ok;running=false;}cv.notify_all(); }
  void fail(const char*s){err(s);{std::lock_guard<std::mutex>l(mutex);failed=true;stopRequested=true;}Block b;b.end=true;if(readyQ)xQueueSend(readyQ,&b,0);}
  void reader(){
    HalFile f=Storage.open(path.c_str(),O_RDONLY);if(!f.isOpen()){fail("Cannot open audio file");return;}
    WavInfo w;if(!parseWav(f,w)){fail("WAV must be PCM/mono/16k/16-bit");return;}
    if(!f.seek(w.dataOffset)){fail("WAV data seek failed");return;}
    LOG_INF(kTag,"WAV stream: path=%s size=%u data=%u rate=%u ch=%u bits=%u",path.c_str(),(unsigned)f.fileSize(),(unsigned)w.dataBytes,(unsigned)w.sampleRate,(unsigned)w.channels,(unsigned)w.bitsPerSample);
    LOG_INF(kTag,"HiFi-DAP style double buffer: count=%u bytes=%u",(unsigned)kBufferCount,(unsigned)kBufferBytes);
    std::size_t remain=w.dataBytes;
    while(remain){if(stopped()){fail("Playback stopped");return;}Block b;if(xQueueReceive(freeQ,&b,pdMS_TO_TICKS(100))!=pdTRUE)continue;auto n=std::min(remain,kBufferBytes);int r=f.read(b.data,n);if(r<=0||(std::size_t(r)&1)){xQueueSend(freeQ,&b,portMAX_DELAY);fail("SD read failed");return;}b.samples=std::size_t(r)/2;b.end=false;if(xQueueSend(readyQ,&b,portMAX_DELAY)!=pdTRUE){xQueueSend(freeQ,&b,portMAX_DELAY);fail("Audio queue failed");return;}remain-=std::size_t(r);}
    Block end;end.end=true;xQueueSend(readyQ,&end,portMAX_DELAY);
  }
  void output(){
    if(!codec.startOutput()){fail("Audio output start failed");finish(false);return;}
    bool ok=true;for(;;){Block b;if(xQueueReceive(readyQ,&b,pdMS_TO_TICKS(100))!=pdTRUE){if(stopped()){ok=false;break;}continue;}if(b.end)break;std::size_t off=0;while(off<b.samples){if(stopped()){ok=false;break;}auto n=codec.write(reinterpret_cast<const int16_t*>(b.data)+off,b.samples-off,1000);if(!n){ok=false;break;}off+=n;}xQueueSend(freeQ,&b,portMAX_DELAY);if(!ok)break;}
    codec.stopOutput();bool wasFailed;{std::lock_guard<std::mutex>l(mutex);wasFailed=failed;}if(wasFailed)finish(false);else if(ok&&!stopped())finish(true);
  }
  bool alloc(){freeQ=xQueueCreate(kBufferCount,sizeof(Block));readyQ=xQueueCreate(kBufferCount,sizeof(Block));if(!freeQ||!readyQ){err("Audio queue allocation failed");return false;}for(std::size_t i=0;i<kBufferCount;i++){buf[i]=static_cast<uint8_t*>(heap_caps_malloc(kBufferBytes,kTaskCaps));if(!buf[i]){err("PSRAM audio buffer allocation failed");return false;}Block b;b.data=buf[i];if(xQueueSend(freeQ,&b,0)!=pdTRUE){err("Audio free queue failed");return false;}}return true;}
  void destroy(){if(readyQ){vQueueDelete(readyQ);readyQ=nullptr;}if(freeQ){vQueueDelete(freeQ);freeQ=nullptr;}for(auto&b:buf)if(b){heap_caps_free(b);b=nullptr;}}
  bool tasks(){if(xTaskCreateWithCaps(rd,"metalio_wav_rd",kTaskStackBytes,this,kReaderPriority,&readerTask,kTaskCaps)!=pdPASS){readerTask=nullptr;err("WAV reader task creation failed");return false;}if(xTaskCreateWithCaps(out,"metalio_wav_out",kTaskStackBytes,this,kOutputPriority,&outputTask,kTaskCaps)!=pdPASS){outputTask=nullptr;err("WAV output task creation failed");return false;}return true;}
  bool start(const char*p){
    if(!p||!*p){err("Invalid audio path");return false;}
    bool busy=false;
    { std::lock_guard<std::mutex>l(mutex); busy=running||readerTask||outputTask; if(!busy){path=p;stopRequested=false;success=false;failed=false;running=true;error[0]=0;} }
    if(busy){err("Audio playback already running");return false;}
    if(!alloc()||!tasks()){requestStop();waitTasks(5000);codec.stopOutput();destroy();{std::lock_guard<std::mutex>l(mutex);running=false;}return false;}
    LOG_INF(kTag,"WAV reader/output tasks started");return true;
  }
  void requestStop(){std::lock_guard<std::mutex>l(mutex);stopRequested=true;}
  bool waitTasks(uint32_t ms){auto d=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);while(readerTask||outputTask){if(std::chrono::steady_clock::now()>=d)return false;vTaskDelay(pdMS_TO_TICKS(10));}return true;}
  void stop(){requestStop();waitTasks(10000);codec.stopOutput();if(!readerTask&&!outputTask){std::lock_guard<std::mutex>l(mutex);running=false;destroy();}cv.notify_all();}
};
Player::Player():impl_(new Impl()){}
Player::~Player(){stop();delete impl_;}
bool Player::playWav(const char*p){return impl_&&impl_->start(p);}
bool Player::wait(uint32_t ms){if(!impl_)return false;std::unique_lock<std::mutex>l(impl_->mutex);bool done=impl_->cv.wait_for(l,std::chrono::milliseconds(ms),[this]{return !impl_->running;});return done&&impl_->success;}
void Player::stop(){if(impl_)impl_->stop();}
bool Player::isPlaying()const{if(!impl_)return false;std::lock_guard<std::mutex>l(impl_->mutex);return impl_->running;}
bool Player::lastSuccess()const{if(!impl_)return false;std::lock_guard<std::mutex>l(impl_->mutex);return impl_->success;}
const char*Player::lastError()const{if(!impl_)return "Player unavailable";std::lock_guard<std::mutex>l(impl_->mutex);return impl_->error;}
} // namespace metalio_audio_file_player
#else
namespace metalio_audio_file_player { Player::Player():impl_(nullptr){} Player::~Player()=default; bool Player::playWav(const char*){return false;} bool Player::wait(uint32_t){return false;} void Player::stop(){} bool Player::isPlaying()const{return false;} bool Player::lastSuccess()const{return false;} const char*Player::lastError()const{return "Metalio E-INK4 only";} }
#endif
