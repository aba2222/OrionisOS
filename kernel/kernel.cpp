// ================== 标准库与外部头文件 ==================
#include <stivale.h>

// ================== 内核公共头文件 ==================
#include "stivale_custom.h"
#include "boot.h"
#include "kernel/drivers/tty.h"
#include "mem/pmm.h"
#include "mem/paging.h"
#include "command/shell.h"

// ================== CPU/中断/定时器/PCI/驱动头文件 ==================
#include "cpu/idt.h"
#include "cpu/isr.h"
#include "cpu/pic.h"
#include "cpu/pit.h"
#include "cpu/pci.h"
#include "kernel/drivers/keyboard.h"
#include "kernel/drivers/ata/ata.h"
#include "kernel/drivers/ethernet/e1000.h"
#include "kernel/drivers/ethernet/virtio_net.h"

// ================== 版本号与全局变量 ==================
const char* KERNEL_VERSION = "1.4.0-dirty+";

// 全局指针，用于保存引导信息，所有模块都可以访问
stivale_struct* boot_info;
bool cursor_visible = true;

// ================== 外部符号声明 ==================
extern "C" void init_gdt();
void set_isr_stivale_struct(stivale_struct* info);

// ================== 静态栈与 stivale header ==================
static uint8_t stack[8192];
__attribute__((section(".stivalehdr"), used))
static struct stivale_header_tag stivale_hdr = {
    .stack = (uintptr_t)stack + sizeof(stack),
    .flags = 1,
    .framebuffer_width  = 0,
    .framebuffer_height = 0,
    .framebuffer_bpp    = 0,
    .entry_point = 0
};

// ================== 内核图形/字符输出相关函数 ==================
void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!boot_info || x >= boot_info->framebuffer_width || y >= boot_info->framebuffer_height) return;
    uint32_t* fb_addr = (uint32_t*)boot_info->framebuffer_addr;
    uint32_t pitch_in_pixels = boot_info->framebuffer_pitch / 4;
    fb_addr[y * pitch_in_pixels + x] = color;
}

void putchar_at(char c, uint32_t x, uint32_t y, uint32_t color) {
    if ((unsigned char)c >= 128) return;
    const unsigned char* glyph = console_tty_9x16 + ((unsigned char)c * 32);
    for (uint32_t row = 0; row < 16; row++) {
        uint16_t row_data = *(uint16_t*)(glyph + row * 2);
        for (uint32_t col = 0; col < 9; col++) {
            if ((row_data >> (8 - col)) & 1) {
                put_pixel(x + col, y + row, color);
            }
        }
    }
}

void draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
    for (uint32_t i = 0; i < height; i++) {
        for (uint32_t j = 0; j < width; j++) {
            put_pixel(x + j, y + i, color);
        }
    }
}

void print(const char* str, uint32_t color) {
    // 1. 在开始任何操作前，用背景色擦除当前的光标
    draw_rect(cursor_x, cursor_y, 9, 16, current_bg_color);
    for (int i = 0; str[i] != '\0'; i++) {
        char c = str[i];
        if (c == '\n') {
            cursor_y += 18;
            cursor_x = 10;
        } else if (c == '\b') {
            if (cursor_x > 10) {
                cursor_x -= 9;
                // 用背景色擦除退格位置的字符
                draw_rect(cursor_x, cursor_y, 9, 16, current_bg_color);
            }
        } else {
            putchar_at(c, cursor_x, cursor_y, color);
            cursor_x += 9;
        }

        // 自动换行
        if (boot_info && (cursor_x + 9 > (uint32_t)(boot_info->framebuffer_width - 10))) {
            cursor_y += 18;
            cursor_x = 10;
        }
    }
    // 结尾重绘光标
    uint32_t cursor_draw_color = cursor_visible ? 0xFFFFFF : current_bg_color;
    draw_rect(cursor_x, cursor_y, 9, 16, cursor_draw_color);
}

// ================== PCI 设备回调示例 ==================
void my_pci_device_callback(uint8_t bus, uint8_t device, uint8_t function, uint16_t vendor_id, uint16_t device_id) {
    if (vendor_id == 0x8086 && (device_id == 0x100E || device_id == 0x100F || device_id == 0x10D3)) {
        tty_print("  Found Intel E1000 Network Card! (Old)\n", 0x0A);
    }
    //  新增：识别 VirtIO 网卡 
    if (vendor_id == 0x1AF4 && (device_id == 0x1000 || device_id == 0x1041)) {
        tty_print("  Found VirtIO Network Card!\n", 0x00FF00);
        virtio_net_init(bus, device, function);
    }
}

// ================== 内核主入口 ==================
extern "C" void kmain(struct stivale_struct *stivale_struct) {
    tty_init(stivale_struct);
    // 关键第一步：将引导信息保存到全局变量
    boot_info = stivale_struct;

    uint32_t white = 0xE0E0E0;
    uint32_t green = 0x4EC9B0;
    uint32_t blue  = 0x569CD6;

    // 清空屏幕
    for (uint32_t y = 0; y < boot_info->framebuffer_height; y++) {
        for (uint32_t x = 0; x < boot_info->framebuffer_width; x++) {
            put_pixel(x, y, current_bg_color);
        }
    }

    tty_print("Kernel loaded!\n", 0xFFFFFF);

    // 初始化 GDT
    print("Initializing GDT...", white);
    init_gdt();
    print("\nGDT loaded.\n", green);

    // 初始化 IDT
    print("Initializing IDT...", white);
    init_idt();
    print("\nIDT loaded.\n", green);

    // 初始化 PIC
    print("Remapping PIC...", white);
    pic_remap(32, 40);
    print("\nPIC remapped.\n", green);

    // 初始化 PIT
    print("Initializing PIT...", white);
    init_pit(1000);
    print("\nPIT at 1000Hz.\n", green);

    // 开启中断
    print("Enabling interrupts (STI)...", white);
    asm volatile ("sti");
    print("\nInterrupts enabled.\n", green);

    print("Initializing Buddy...", white);
    buddy_init(boot_info);
    print("\nBuddy ready.\n", green);

    print("Init paging...", white);
    initPML4();
    print("\nPaging initialized.\n", green);

    // 初始化 Keyboard
    print("Initializing Keyboard...", white);
    init_keyboard();
    pic_unmask_irq(1);
    pic_unmask_irq(2);
    print("\nKeyboard ready.\n", green);

    pic_unmask_irq(10);

    tty_print("Scrolling: ", 0xFFFF00);
    tty_print("FB Height=", 0xFFFFFF);
    print_hex(boot_info->framebuffer_height, 0xFFFFFF);
    tty_print(" Pitch=", 0xFFFFFF); 
    print_hex(boot_info->framebuffer_pitch, 0xFFFFFF);
    tty_print(" Scroll=", 0xFFFFFF); 
    
    tty_print("\nInitializing ATA driver...", 0xFFFFFF);
    ata_init();
    tty_print("\nATA driver ready.\n", 0x4EC9B0);

    tty_print("Reading MBR (LBA 0)...", 0xFFFFFF);
    uint8_t mbr_buffer[512]; // 512字节的缓冲区
    if (ata_read_sectors(0, 0, 1, mbr_buffer)) { // master drive, LBA 0, 1 sector
        uint16_t mbr_signature = (uint16_t)mbr_buffer[510] | ((uint16_t)mbr_buffer[511] << 8);
        tty_print("\nMBR read successful. Signature: 0x", 0x00FF00);
        print_hex(mbr_signature, 0x00FFFF); // 打印整个 16 位的签名
        tty_print("\n", 0x00FFFF);
    } else {
        tty_print("MBR read failed!\n", 0xFF0000);
    }

    tty_print("Initializing PCI bus...\n", 0xFFFFFF);
    pci_init();
    pci_scan_bus(my_pci_device_callback);
    tty_print("PCI bus scan done.\n", 0x4EC9B0);


    tty_print("\n-----Testing Paging-----\n", 0x569CD6);
    // 映射虚拟地址到物理地址
    mapPage((void*)0x9000, (void*) 0x1000, 3);
    mapPage((void*)0xA000, (void*) 0x1000, 3);

    // 获取映射后的物理地址
    uint64_t* p = getPhysicalAddress((void*) 0x9000);
    uint64_t* f = getPhysicalAddress((void*) 0xA000);

    // 写入虚拟地址 0x9000，并读取
    uint64_t* y = (uint64_t*) 0x9000;
    *y = 0x114514;
    print_hex(*y, 0xFFFFFF); // 若分页运行正常，应该输出*y的值
    tty_print("\n", 0xFFFFFF);

    uint64_t* z = (uint64_t*) 0xA000;
    *z = 0x1919810;
    print_hex(*z, 0xFFFFFF); // 若分页运行正常，应该输出*z的值
    tty_print("\n", 0xFFFFFF);

    // 输出虚拟地址映射到的物理地址
    print_hex((uint64_t)p, 0xFFFFFF);
    tty_print("\n", 0xFFFFFF);
    print_hex((uint64_t)f, 0xFFFFFF);
    tty_print("\n", 0xFFFFFF);

    // 释放分页映射
    tty_print("\nReleasing paging...\n", 0xFF6347);
    unmapPage((void*) 0x9000); // 释放虚拟地址 0x9000 的映射
    unmapPage((void*) 0xA000); // 释放虚拟地址 0xA000 的映射

    // 释放后再访问虚拟地址，检查是否出错
    uint64_t* p_after_unmap = getPhysicalAddress((void*) 0x9000);
    uint64_t* f_after_unmap = getPhysicalAddress((void*) 0xA000);

    print_hex((uint64_t)p_after_unmap, 0xFF6347);
    tty_print("\n", 0xFFFFFF);
    print_hex((uint64_t)f_after_unmap, 0xFF6347);
    tty_print("\n", 0xFFFFFF);

    tty_print("\nPaging test done.\n", 0x4EC9B0);

    print("[System ready.]\n", blue);

    init_shell();

    pic_dump_masks();
    
    bool last_cursor_state = !cursor_visible; // 强制第一次循环时重绘

     for (;;) {
        asm ("hlt"); // 等待下一次中断
    }
}