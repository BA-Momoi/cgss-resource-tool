/*
* 针对CGSS的usm解包程序 
* 用法： usm.exe <输入usm> [输出目录]
* 默认当前目录的demux_out文件夹
* 输出:
* video.m2v
* audio.adx(有音频时才会出现)
*/
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<direct.h>
#include<windows.h>

static unsigned be32(const unsigned char *p){   
    return ((unsigned)p[0] <<24) | ((unsigned)p[1] << 16)
         | ((unsigned)p[2] << 8) | p[3];
}
static unsigned be16(const unsigned char *p){   
    return ((unsigned)p[0] << 8) | p[1] ;
}

static unsigned char videoMask1[0x20];
static unsigned char videoMask2[0x20];
static unsigned char audioMask[0x20];
// 我也不知道原理，大佬写的cpp脚本抄的
static void InitMask(unsigned int key1,unsigned int key2){
    unsigned char t[0x20];
    t[0x00] = ((unsigned char *)&key1)[0];
    t[0x01] = ((unsigned char *)&key1)[1];
    t[0x02] = ((unsigned char *)&key1)[2];
    t[0x03] = ((unsigned char *)&key1)[3] - 0x34;
    t[0x04] = ((unsigned char *)&key2)[0] + 0xF9;
    t[0x05] = ((unsigned char *)&key2)[1] ^ 0x13;
    t[0x06] = ((unsigned char *)&key2)[2] + 0x61;
    t[0x07] = t[0x00] ^ 0xFF;
    t[0x08] = t[0x02] + t[0x01];
    t[0x09] = t[0x01] - t[0x07];
    t[0x0A] = t[0x02] ^ 0xFF;
    t[0x0B] = t[0x01] ^ 0xFF;
    t[0x0C] = t[0x0B] + t[0x09];
    t[0x0D] = t[0x08] - t[0x03];
    t[0x0E] = t[0x0D] ^ 0xFF;
    t[0x0F] = t[0x0A] - t[0x0B];
    t[0x10] = t[0x08] - t[0x0F];
    t[0x11] = t[0x10] ^ t[0x07];
    t[0x12] = t[0x0F] ^ 0xFF;
    t[0x13] = t[0x03] ^ 0x10;
    t[0x14] = t[0x04] - 0x32;
    t[0x15] = t[0x05] + 0xED;
    t[0x16] = t[0x06] ^ 0xF3;
    t[0x17] = t[0x13] - t[0x0F];
    t[0x18] = t[0x15] + t[0x07];
    t[0x19] = 0x21 - t[0x13];
    t[0x1A] = t[0x14] ^ t[0x17];
    t[0x1B] = t[0x16] + t[0x16];
    t[0x1C] = t[0x17] + 0x44;
    t[0x1D] = t[0x03] + t[0x04];
    t[0x1E] = t[0x05] - t[0x16];
    t[0x1F] = t[0x1D] ^ t[0x13];

    unsigned char t2[4] = {'U','R','U','C'};
    for (int i = 0; i < 0x20; i++){
        videoMask1[i] = t[i];
        videoMask2[i] = t[i] ^ 0xFF;
        audioMask[i]  = (i & 1) ? t2[(i >> 1) & 3] : t[i] ^ 0xFF;
    }
}

static void MaskVideo(unsigned char *data,int size){
    data += 0x40;
    size -= 0x40;
    
    if(size < 0x200) return ;
     unsigned char mask[0x20];
    /* 第一遍: 从 0x100 到最后, mask 带反馈 */
    memcpy(mask, videoMask2, 0x20);
    for (int i = 0x100; i < size; i++){
        data[i] ^= mask[i & 0x1F];
        mask[i & 0x1F] = data[i] ^ videoMask2[i & 0x1F];
    }
    /* 第二遍: 前 0x100 字节 */
    memcpy(mask, videoMask1, 0x20);
    for (int i = 0; i < 0x100; i++){
        mask[i & 0x1F] ^= data[0x100 + i];
        data[i] ^= mask[i & 0x1F];
    }
}

static void MaskAudio(unsigned char *data,int size){
    data += 0x140;
    size -= 0x140;
    for (int i = 0; i < size; i++){
        data[i] ^= audioMask[i & 0x1F];
    }
}

static void make_dir(const char *dir){
    mkdir(dir);
}

int main(int argc,char *argv[]){
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    if(argc < 2){
        fprintf(stderr,"用法: usm_demux_example.exe <输入.usm> [输出目录]\n");
        fflush(stdout);    // 先把结果全部输出，再等按键，避免重定向时和 pause 混在一起
        system("pause");   // 双击 exe 时窗口不闪退，按任意键退出
        return 1;
    }
    const char *infile = argv[1];
    char outdir[1024];
    if(argc >= 3){
        snprintf(outdir,sizeof outdir,"%s",argv[2]);
    }
    else{
        snprintf(outdir,sizeof outdir,"demux_out");
    }
    make_dir(outdir);
    InitMask(0XF27E3B22,0x00003657);

    FILE *fp = fopen(infile, "rb");
    if(!fp){
        fprintf(stderr,"打开%s文件失效\n", infile);
        return 1;
    }
    fseek(fp,0,SEEK_END);
    long fileSize = ftell(fp);

    char vpath[1024],apath[1024];
    snprintf(vpath,sizeof vpath,"%s\\videoo.m2v",outdir);
    snprintf(apath,sizeof apath,"%s\\audio.adx",outdir);
    
    FILE *vo = NULL;
    FILE *ao = NULL;
    long pos = 0;
    unsigned nchunk = 0, nvideo = 0, naudio = 0;
    while(pos <= fileSize){
        unsigned char h[32];
        fseek(fp,pos,SEEK_SET);
        if(fread(h,1,32,fp) != 32) break;
        unsigned ds = be32(h + 4);
        unsigned dof = h[9];
        unsigned pad = be16(h + 10);
        unsigned typ = h[15] & 3;   //00000011

        unsigned dlen = ds - dof - pad;
        unsigned dpos = pos + 8 + dof;

        unsigned char *data = (unsigned char *)malloc(dlen);
        fseek(fp,dpos,SEEK_SET);
        fread(data,1,dlen,fp);

        if(memcmp(h,"@SFV",4) == 0 && typ == 0){
        /* 视频数据块: 解密后连续写进 video.m2v */
            MaskVideo(data, dlen);
            if (!vo) vo = fopen(vpath, "wb");
            fwrite(data, 1, dlen, vo);
            nvideo++;
        } else if (memcmp(h, "@SFA", 4) == 0 && typ == 0){
            /* 音频数据块: 解密后连续写进 audio.adx */
            MaskAudio(data, dlen);
            if (!ao) ao = fopen(apath, "wb");
            fwrite(data, 1, dlen, ao);
            naudio++;
        }
        free(data);
        nchunk++;
        pos += 8 + ds;   
    }
    fclose(fp);
    if(vo) fclose(vo);
    if(ao) fclose(ao);
    printf("完成: 共 %u 个块, 视频块 %u 个, 音频块 %u 个\n",
           nchunk, nvideo, naudio);
    printf("输出目录: %s\n", outdir);
    return 0;
}