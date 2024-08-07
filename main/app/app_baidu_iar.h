#ifndef _APP_BAIDU_IAR_H_
#define _APP_BAIDU_IAR_H_

#ifdef __cplusplus
extern "C"
{
#endif
    typedef struct
    {
        char *format;
        int rate;
        int channel;
        char *cuid;
        char *token;
        char *speech; /*音频文件，读取二进制内容后，进行 base64 编码后放在 speech 参数内*/
        int len;      /*音频文件的原始大小, 即二进制内容的字节数，填写 “len” 字段*/
    } iar_payload_t;

    void app_baidu_iar_init(void);
#ifdef __cplusplus
}
#endif

#endif