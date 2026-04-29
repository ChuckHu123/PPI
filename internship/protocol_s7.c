#include "protocol_s7.h"
#include "protocol_base.h"
#include "protocol_utils.h"

#include <string.h>
#include <stdlib.h>
#include <rtdbg.h>

int validate_s7_connect(const uint8_t *data, uint32_t len, bool *complete_data, uint16_t *offset)
{
    if (len < 22)//第一次好像固定是22，第二次收25，发27
        return 0;

    int i = 0;
    for (i; i <= len - 3; i++)
    {
        if (data[i] == 0x03)
        {
            if (data[i + 3] == 0x16 && data[i + 5] == 0xE0)// 第一次握手
            {
                *complete_data = true;
                *offset = i;
                return i + 22;
            }
            else if (data[i + 3] == 0x19 && data[i + 5] == 0xF0 && data[8] == 0x01)// 第二次握手
            {
                *complete_data = true;
                *offset = i;
                return i + 25;
            }
        }
    }
    return i;
}

int validate_s7_req(const uint8_t *data, uint32_t len, bool *complete_data, uint16_t *offset, uint16_t *data_len)
{
    if (len < 30)
        return 0;

    int i = 0;
    for (i; i <= len - 3; i++)
    {
        if (data[i] == 0x03)
        {
            *data_len = data[i + 3];
            if (i + *data_len > len)
                return i;// 后续数据长度不够,返回前面的无用报文
            else
            {
                if (data[i + 5] == 0xF0 && data[i + 7] == 0x32 && data[i + 8] == 0x01)
                {
                    *complete_data = true;
                    *offset = i;
                    return i + *data_len;
                }
            }
        }
    }
    return i;
}

void build_s7_connect_res(const uint8_t *data, uint32_t len, uint16_t offset, uint8_t *res_data, int state)
{
    if (state == S7_STATE_IDLE)
    {
        memcpy(res_data, data + offset, 22);
        res_data[5] = 0xD0;
        memcpy(&res_data[6], &res_data[8], 2);
        res_data[8] = 0x00;
        res_data[9] = 0x03;// src-ref不知道是多少，先随便写一个
    }
    else
    {
        memcpy(res_data, data + offset, 25);
        memmove(&res_data[19], &res_data[17], 8);
        res_data[3] = 0x1B;
        res_data[8] = 0x03;
        res_data[17] = 0x00;
        res_data[18] = 0x00;
    }
}

/**
 * @brief 解析s7的读写报文，转成ppi请求报文
 * @param data s7报文
 * @param len s7报文长度
 * @param ppi_data 转换后的ppi报文
 */
void on_parse_s7(const uint8_t *data, uint32_t len, uint8_t *ppi_data)
{
    if (data[17] == 0x04)//读
    {
        uint8_t read_type = data[22]; // 读类型 bit/byte
        uint8_t read_len = data[24]; // 读长度
        uint8_t storage_code = data[27];
        uint32_t addr = (data[28] << 16 | data[29] << 8 | data[30]) / 8;
        //LOG_I("S7 READ: type=%d, len=%d, storage_code=0x%02X, addr=0x%08X", read_type, read_len, storage_code, addr);
        //LOG_HEX("S7 READ data", 32, data, len);

        //转成PPI报文
        uint8_t temp_data[33] = {
            0x68, 0x1B, 0x1B, 0x68, 0x02, 0x00, 0x6C, 0x32, 0x01, 0x00, //固定
            0x00, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x04, 0x01, 0x12, 0x0A, 0x10, //固定
            read_type, 0x00, read_len, 0x00,
            storage_code == 0x84 ? 0x01 : 0x00, storage_code, (addr * 8) >> 16 & 0xFF, (addr * 8) >> 8 & 0xFF, (addr * 8) & 0xFF,// 地址
            0x00, 0x16
        };
        temp_data[31] = calculate_fcs(temp_data, 4, 30);
        memcpy(ppi_data, temp_data, 33);
        //LOG_HEX("CONVERTED PPI data", 32, temp_data, 33);
    }
    else if (data[17] == 0x05)//写
    {
        uint8_t write_type = data[22];
        uint8_t write_len = data[24];
        uint8_t storage_code = data[27];
        uint32_t addr = (data[28] << 16 | data[29] << 8 | data[30]) / 8;
        //LOG_I("S7 WRITE: type=%d, len=%d, storage_code=0x%02X, addr=0x%08X, data=", write_type, write_len, storage_code, addr);
        //LOG_HEX("S7 WRITE data", 32, data, len);
        uint8_t ppi_data_len = 35 + write_len + 2;

        uint16_t bit_count;
        if (write_type == 1)
            bit_count = write_len;
        else if (write_type == 2)
            bit_count = write_len * 8;
        else if (write_type == 4)
            bit_count = write_len * 16;
        else if (write_type == 6)
            bit_count = write_len * 32;
        else
            LOG_W("Unknown S7 data type: 0x%02X", write_type);

        uint8_t temp_data[35] = {
            0x68, 31 + write_len, 31 + write_len, 0x68, 0x02, 0x00, 0x6C, 0x32, 0x01, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x0E, 0x00,
            4 + write_len, 0x05, 0x01, 0x12, 0x0A, 0x10,
            write_type, 0x00, write_len, 0x00,
            storage_code == 0x84 ? 0x01 : 0x00, storage_code, (addr * 8) >> 16 & 0xFF, (addr * 8) >> 8 & 0xFF, (addr * 8) & 0xFF,
            0x00, write_type == 1 ? 0x03 : 0x04, bit_count >> 8 & 0xFF, bit_count & 0xFF
        };
        memcpy(ppi_data, temp_data, 35);
        memcpy(ppi_data + 35, data + 35, write_len);//复制数据位
        ppi_data[ppi_data_len - 2] = calculate_fcs(ppi_data, 4, ppi_data_len - 3);
        ppi_data[ppi_data_len - 1] = 0x16;
        //LOG_HEX("CONVERTED PPI data", 32, ppi_data, ppi_data_len);
    }
    else
    {
        LOG_W("In on_parse_s7, Unknown S7 function: 0x%02X", data[17]);
    }
}

void convert_ppi_to_s7(const uint8_t *data, uint32_t len, uint8_t *s7_data, uint32_t s7_data_len, uint16_t unit_ref)
{
    if (data[19] == 0x04)//读
    {
        if (data[21] != 0xFF)
            LOG_W("In convert_ppi_to_s7, PLC READ response with error");
        else
        {
            uint16_t data_len = s7_data_len - 25;// 数据长度
            uint8_t data_type = data[22];
            uint16_t data_qty = data[23] << 8 | data[24];//非T/C/HC区为有效bit数，反之为byte

            uint8_t temp_data[25] = {
                0x03, 0x00, s7_data_len >> 8 & 0xFF, s7_data_len & 0xFF, 0x02, 0xF0, 0x80, 0x32,
                0x03, 0x00, 0x00, unit_ref >> 8 & 0xFF, unit_ref & 0xFF, 0x00, 0x02,
                (4 + data_len) >> 8 & 0xFF, (4 + data_len) & 0xFF, 0x00, 0x00, 0x04, 0x01, 0xFF,
                data_type, data_qty >> 8 & 0xFF, data_qty & 0xFF
            };
            memcpy(s7_data, temp_data, 25);

            if (data_type == 0x03)// bit型，只能读一位
            {
                uint8_t read_data = data[25] & 0x01; // 只读一位
                memcpy(s7_data + 25, &read_data, 1);
            }
            else if (data_type == 0x04)// Byte，Word，Dword型
            {
                data_qty = data_qty / 8; // bit数转byte数
                for (int i = 0; i < data_qty; i++)
                {
                    uint8_t read_data = data[25 + i];
                    memcpy(s7_data + 25 + i, &read_data, 1);
                }
            }
            else
                LOG_W("S7 READ: unknown data type: 0x%02X", data_type);
        }
    }
    else if (data[19] == 0x05)//写
    {
        if (data[21] != 0xFF)
            LOG_W("PLC WRITE response with error");
        else
        {
            uint8_t temp_data[22] = {
                0x03, 0x00, 0x00, 0x16, 0x02, 0xF0, 0x80, 0x32,
                0x03, 0x00, 0x00, unit_ref >> 8 & 0xFF, unit_ref & 0xFF, 0x00, 0x02, 0x00,
                0x01, 0x00, 0x00, 0x05, 0x01, 0xFF
            };
            memcpy(s7_data, temp_data, 22);
        }
    }
    else
        LOG_W("In convert_ppi_to_s7, Unknown S7 function: 0x%02X", data[19]);
}

void build_s7_error_response(const uint8_t *data, uint32_t len, uint16_t unit_ref, uint8_t *e_data, uint8_t rw_type)
{
    uint8_t error_type = 0x84;// 错误类型
    uint8_t error_code = 0x04;// 错误码
    if (rw_type == 0x04)
    {
        uint8_t temp_data[25] = {
            0x03, 0x00, 0x00, 0x19, 0x02, 0xF0, 0x80, 0x32,
            0x03, 0x00, 0x00, unit_ref >> 8 & 0xFF, unit_ref & 0xFF, 0x00, 0x02, 0x00,
            0x04, error_type, error_code,
            rw_type, 0x01, 0x05, 0x00, 0x00, 0x00
        };
        memcpy(e_data, temp_data, 25);
    }
    else if (rw_type == 0x05)
    {
        uint8_t temp_data[22] = {
            0x03, 0x00, 0x00, 0x16, 0x02, 0xF0, 0x80, 0x32,
            0x03, 0x00, 0x00, unit_ref >> 8 & 0xFF, unit_ref & 0xFF, 0x00, 0x02, 0x00,
            0x01, error_type, error_code, 0x05, 0x01, 0x05
        };
        memcpy(e_data, temp_data, 22);
    }
    else
        LOG_W("In build_s7_error_response, Unknown function code: 0x%02X", rw_type);
}

