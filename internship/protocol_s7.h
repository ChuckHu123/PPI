#ifndef PROTOCOL_S7_H
#define PROTOCOL_S7_H

#include <stdint.h>
#include <stdbool.h>

// S7协议状态机
typedef enum
{
    S7_STATE_IDLE = 0,
    S7_STATE_WAIT_ACK,
    S7_STATE_CONFIRMED,// 也视作空闲状态（连接成功的空闲）
    S7_TO_PLC_STATE_WAIT_ACK,
    S7_TO_PLC_STATE_WAIT_RESP,
} s7_state_t;

// S7协议握手验证
int validate_s7_connect(const uint8_t *data, uint32_t len, bool *complete_data, uint16_t *offset);

// S7协议请求验证
int validate_s7_req(const uint8_t *data, uint32_t len, bool *complete_data, uint16_t *offset, uint16_t *data_len);

// 构建S7连接响应
void build_s7_connect_res(const uint8_t *data, uint32_t len, uint16_t offset, uint8_t *res_data, int state);

// 解析S7读写报文并转换为PPI格式
void on_parse_s7(const uint8_t *data, uint32_t len, uint8_t *ppi_data);

// 将PPI响应转换为S7格式
void convert_ppi_to_s7(const uint8_t *data, uint32_t len, uint8_t *s7_data, uint32_t s7_data_len, uint16_t unit_ref);

// 构建S7错误响应
void build_s7_error_response(const uint8_t *data, uint32_t len, uint16_t unit_ref, uint8_t *e_data, uint8_t rw_type);

#endif // PROTOCOL_S7_H


