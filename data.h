#include<stdbool.h>
#ifndef _DATA_H_
#define _DATA_H_

typedef struct 
{
    int id;
    char name[128];
    int bpm;
    char composer[64];
    bool music_exclude;
} musicInfo;

typedef struct
{
    long id;    //卡片id
    char name[128];  //卡片名
    int chara_id;   //角色id
    int rarity;     //稀有度
    int attribute;  //属性
    int title_flag; //称号卡标记    0=普通卡    1=称呼卡
    int series_id;  //系列id
    long evolution_id;   //觉醒卡片id
    int evolution_type;  //进化/卡类型
    int place;  //卡面地点id
    int album_id;   //图鉴id
    int solo_live;  //Solo演出标记 非0为有 值=配置id
    int open_story_id;  //剧情id
    int open_dress_id;  //服装id

}caraInfo;

#endif