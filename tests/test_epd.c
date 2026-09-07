#include "epd.h"
#include "os/os.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int gpio[32], bits, value, command, low_reads, stuck;
static unsigned counts[256], ncmd[256], wrong_old, new_count;
static uint8_t new_data[16], window[7];
static unsigned window_count;
static unsigned sleep_key;
static os_time_t ticks;
os_time_t os_time_get(void) { return ticks; }
void os_time_delay(os_time_t n) { ticks += n; }
int hal_gpio_init_out(int pin, int v) { gpio[pin]=v; return 0; }
int hal_gpio_init_in(int pin, int pull) { assert(pin==4); return 0; }
int hal_gpio_read(int pin) { assert(pin==4); if(stuck) return 0; return low_reads-- <= 0; }
void hal_gpio_write(int pin, int v) {
    if(pin==1 && !v) { bits=0; value=0; }
    if(pin==0 && v && !gpio[0] && !gpio[1]) {
        value=(value<<1)|gpio[30]; ++bits;
    }
    if(pin==1 && v && !gpio[1]) {
        assert(bits==8);
        if(!gpio[2]) { command=value; ++ncmd[value]; }
        else {
            ++counts[command];
            if(command==0x07) sleep_key=value;
            if(command==0x90) { assert(window_count<7); window[window_count++]=value; }
            if(command==0x10 && value!=255) ++wrong_old;
            if(command==0x13) { assert(new_count<16); new_data[new_count++]=value; }
        }
    }
    gpio[pin]=v;
}
int main(void) {
    epd_gpio_init(); assert(gpio[5]==0 && gpio[1]==1 && gpio[3]==1);
    low_reads=2;
    assert(epd_begin()==0);
    assert(ncmd[0x06]==1 && counts[0x06]==3 && counts[0x00]==2 && counts[0x61]==3);
    assert(ncmd[0x71]==3 && counts[0x10]==2756 && !wrong_old && ncmd[0x13]==1);
    const uint8_t data[]={0x80,0x01,0xa5,0x00,0xff};
    assert(epd_write(data,sizeof data)==0 && new_count==sizeof data);
    assert(!memcmp(data,new_data,sizeof data));
    os_time_t before=ticks;
    assert(epd_refresh()==0 && ncmd[0x12]==1 && ticks>before);
    assert(epd_off()==0 && ncmd[0x02]==1 && ncmd[0x07]==1 && sleep_key==0xa5);
    assert(gpio[1]==1 && gpio[30]==0 && gpio[2]==0);
    before=ticks;
    assert(epd_off()==0 && ticks==before && ncmd[0x02]==1 && ncmd[0x07]==1);
    assert(epd_begin_partial(1,0,8,2)!=0);
    new_count=0;low_reads=0;
    assert(epd_begin_partial(8,210,16,2)==0);
    const uint8_t expected_window[]={8,23,0,210,0,211,0x28};
    assert(window_count==7 && !memcmp(window,expected_window,7));
    assert(counts[0x20]==44 && counts[0x21]==42 && counts[0x24]==42);
    const uint8_t partial[]={255,0,0xaa,0x55,0xf0,0x0f,0x33,0xcc};
    assert(epd_write(partial,8)==0 && new_count==4);
    for(unsigned i=0;i<4;i++) assert(new_data[i]==(uint8_t)(partial[i+4]^255));
    assert(epd_refresh()==0 && epd_off()==0);
    stuck=1; ticks=UINT32_MAX-10; before=ticks;
    assert(epd_begin()!=0 && (os_time_t)(ticks-before)>=15*128);
    assert((os_time_t)(ticks-before)<16*128 && ncmd[0x13]==2);
    unsigned sleeps=ncmd[0x07];
    assert(epd_off()!=0 && ncmd[0x07]==sleeps);
    puts("epd: GPIO SPI bytes, 2756-byte old plane, partial LUT/window/polarity, BUSY timeout and tick wrap passed");
}
