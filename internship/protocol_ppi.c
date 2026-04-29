#include "protocol_base.h"
#include "protocol_s7.h"
#include "protocol_utils.h"

#include <stdint.h>
#define DBG_TAG "protocol_ppi"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

// PPI存储区对照表条目
typedef struct
{
    uint8_t storage_code;        // 存储区代码
    const char *name;            // 存储区名称
} ppi_storage_entry_t;

// PPI存储区对照表（按代码排序便于查找）
static ppi_storage_entry_t s_ppi_storages[] = {
    { 0x04, "S" },   // 顺序继电器
    { 0x05, "SM" },   // 特殊内部继电器
    { 0x06, "AIW" },  // 模拟量输入字
    { 0x07, "AQW" },  // 模拟量输出字
    { 0x1E, "C" },   // 计数器
    { 0x1F, "T" },   // 定时器
    { 0x20, "HC" },  // 高速计数器
    { 0x81, "I" },   // 输入映像寄存器
    { 0x82, "Q" },   // 输出映像寄存器
    { 0x83, "M" },   // 内部标志位存储器
    { 0x84, "V" },  // 变量存储器
};

// PPI解析后的地址信息
typedef struct
{
    uint32_t address;                    // 地址偏移
    const ppi_storage_entry_t *entry;    // 指向对照表条目（可为NULL）
} ppi_addr_info_t;

// PPI协议状态机
typedef enum
{
    PPI_STATE_IDLE = 0,        // 空闲状态
    PPI_STATE_WAIT_ACK,        // 等待PLC的0xE5确认
    PPI_STATE_WAIT_SECOND_REQ, // 等待HMI的第二次请求
    PPI_STATE_WAIT_FINAL_RESP, // 等待PLC的最终响应
} protocol_state_t;

// PPI读写状态
typedef enum
{
    PPI_READ = 0,
    PPI_WRITE = 1,
    PPI_FORCE_UNFORCE = 2,
} ppi_rw_t;

// 读写变量类型  暂时未实现C/T/HC区 1E:读写C区 ； 1F:读写T区；20：读HC区（HC不能写）
typedef enum
{
    BIT = 0,
    BYTE = 1,
    WORD = 2,
    DWORD = 3,
    C = 4,
    T = 5,
    HC = 6,
} ppi_data_type_t;

// 协议类型UNDEFINE=0 /PPI=1 /S7=2(优先判定s7，s7只会走网口进来)
typedef enum
{
    UNDEFINE = 0,
    PPI = 1,
    S7 = 2,
} protocol_type_t;

typedef struct
{
    ppi_rw_t rw_type;          // 读写状态
    ppi_data_type_t data_type; // 读写变量类型
    protocol_type_t protocol_type; // 协议类型
    s7_state_t s7_state;         // S7协议当前状态
    uint8_t unit_ref;              // unit reference
} ppi_req_ctx_t;

// PPI上下文结构体
typedef struct
{
    protocol_state_t state;         // PPI协议当前状态
    //uint8_t data[256];         // 数据
    //ppi_addr_info_t addr;           // 存储区地址
    ppi_req_ctx_t req_ctx[COM_MAX_PORT - 1];     // 存储请求com口的相关内容
} ppi_ctx_t;

int protocol_ppi_on_write(void *ctx, uint32_t txn_id, int com, const uint8_t *data, uint32_t len);

void *protocol_ppi_init(const char *arg)
{
    LOG_I("protocol_ppi_init: %s", arg);
    ppi_ctx_t *ctx = (ppi_ctx_t *)rt_malloc(sizeof(ppi_ctx_t));
    if (ctx == NULL)
    {
        LOG_E("protocol_ppi_init: malloc failed");
        return NULL;
    }
    memset(ctx, 0, sizeof(ppi_ctx_t));
    return ctx;
}

void protocol_ppi_deinit(void *ctx)
{
    LOG_I("protocol_ppi_deinit");
    if (ctx)
    {
        rt_free(ctx);
    }
}

void protocol_ppi_on_connected(void *ctx, int com)
{
    ppi_ctx_t *ppi_ctx = (ppi_ctx_t *)ctx;
    LOG_I("protocol_ppi_on_connected: %d", com);

    // 重置状态，准备新的通信
    memset(&ppi_ctx->req_ctx[com - 1], 0, sizeof(ppi_req_ctx_t));
}

void protocol_ppi_on_disconnected(void *ctx, int com)
{
    ppi_ctx_t *ppi_ctx = (ppi_ctx_t *)ctx;
    LOG_I("protocol_ppi_on_disconnected: %d", com);
    memset(&ppi_ctx->req_ctx[com - 1], 0, sizeof(ppi_req_ctx_t));
}

// 验证PPI请求1的完整性
static int validate_ppi_req1(const uint8_t *data, uint32_t len, bool *complete_data, uint16_t *data_len, uint16_t *offset)
{
    if (len < 6)
    {  // 最小能进行判断的帧长度
        return 0;//长度不够继续等待
    }

    int i = 0;
    for (i; i <= len - 4; i++)
    {
        if (data[i] == 0x68 && data[i + 3] == 0x68 && data[i + 1] == data[i + 2])// 开头四位正确
        {
            *data_len = data[i + 1];
            if (i + 6 + *data_len > len)// 开头四字节+末尾2字节+data_len长度
            {
                return i;// 后续数据长度不够,返回前面的无用报文
            }
            else// 长度够了
            {
                if (data[i + 7] != 0x32 || data[i + 8] != 0x01 ||
                    data[i + 5 + *data_len] != 0x16 || // 结尾字节不对
                    data[i + 4 + *data_len] != calculate_fcs(data, i + 4, i + 3 + *data_len))// 校验和不对
                {
                    continue;// 结尾与校验不对，继续找
                }
                else
                {
                    *complete_data = true;
                    *offset = i;
                    return i + 6 + *data_len;
                }
            }
        }
    }
    return i;//无用报文
}

// 验证PPI请求2的完整性
static int validate_ppi_req2(const uint8_t *data, uint32_t len, bool *complete_data, uint16_t *offset)
{
    if (len < 6)
    {
        return 0;
    }
    int i = 0;
    for (i; i <= len - 6; i++)
    {
        if (data[i] == 0x10 && data[i + 3] == 0x5C && data[i + 5] == 0x16)
        {
            if (calculate_fcs(data, i + 1, i + 3) == data[i + 4])
            {
                *complete_data = true;
                *offset = i;
                return i + 6;
            }
        }
    }
    return i;
}

// 验证PPI响应2的完整性
static int validate_ppi_res2(const uint8_t *data, uint32_t len, bool *complete_data, uint16_t *data_len, uint16_t *offset)
{
    if (len < 6)
    {
        return 0;
    }

    int i = 0;
    for (i; i <= len - 4; i++)
    {
        if (data[i] == 0x68 && data[i + 3] == 0x68 && data[i + 1] == data[i + 2])
        {
            *data_len = data[i + 1];
            if (i + 6 + *data_len > len)//开头四字节+末尾2字节+data_len长度
            {
                return i;// 后续数据长度不够,返回前面的无用报文
            }
            else// 长度够了
            {
                if (data[i + 6] != 0x08 || data[i + 7] != 0x32 || data[i + 8] != 0x03 ||
                    data[i + 5 + *data_len] != 0x16 ||
                    data[i + 4 + *data_len] != calculate_fcs(data, i + 4, i + 3 + *data_len))
                {
                    continue;
                }
                else
                {
                    *complete_data = true;
                    *offset = i;
                    return i + 6 + *data_len;
                }
            }
        }
    }
    return i;
}

// 查找PPI存储区信息
static const ppi_storage_entry_t *ppi_lookup_storage(uint8_t storage_code)
{
    for (size_t i = 0; i < sizeof(s_ppi_storages) / sizeof(s_ppi_storages[0]); i++)
    {
        if (s_ppi_storages[i].storage_code == storage_code)
        {
            return &s_ppi_storages[i];
        }
    }
    return NULL;
}

// 取ppi报文中的地址
static void get_ppi_addr(void *ctx, uint32_t len, const uint8_t *data)
{
    uint8_t storage_code = data[27] & 0xFF;

    ppi_addr_info_t addr_info = { (data[28] << 16 | data[29] << 8 | data[30]) / 8, NULL };
    addr_info.entry = ppi_lookup_storage(storage_code);

    if (addr_info.entry)
    {
        LOG_D("get_ppi_addr: 0x%02X, %d", addr_info.entry->storage_code, addr_info.address);//地址以16进制打印
    }
    else
    {
        LOG_W("Unknown storage: 0x%02X", storage_code);
    }
}

// 取ppi报文中的值,未适配s7
static void get_ppi_value(void *ctx, uint32_t len, int com, const uint8_t *data, ppi_rw_t rw_type)
{
    ppi_ctx_t *ppi_ctx = (ppi_ctx_t *)ctx;
    if (rw_type == PPI_READ)
    {
        // 读数据
        uint8_t data_type = data[22];
        uint16_t data_qty = data[23] << 8 | data[24]; // 有效数据数量，非T/C/HC区为有效bit数，反之为byte
        if (data_type == 0x03)// bit型，只能读一位
        {
            uint8_t read_data = data[25] & 0x01; // 只读一位
            LOG_D("READ: bit, %d; com: %d", read_data, com);
        }
        else if (data_type == 0x04)// Byte，Word，Dword型
        {
            data_qty = data_qty / 8; // bit数转byte数
            LOG_D("READ: byte, qty=%d ; com: %d; value=", data_qty, com);
            for (int i = 0; i < data_qty; i++)
            {
                uint8_t read_data = data[25 + i]; // 只读一位
                LOG_D("0x%02X ", read_data); // 打印byte值
            }
        }
        else if (data_type == 0x09)// T,C,HC区数据
        {
            if ((ppi_ctx->req_ctx[com - 1].data_type == T && data[25] == 0x02) || // T,C均为Word类型
                (ppi_ctx->req_ctx[com - 1].data_type == C && data[25] == 0x10))
            {
                for (int i = 0; i < data_qty; i++)
                {
                    uint16_t read_data = data[26 + i * 2] << 8 | data[27 + i * 2];
                    LOG_D("READ: word, %d; com: %d", read_data, com);
                }
            }
            else if (ppi_ctx->req_ctx[com - 1].data_type == HC)// HC为Dword类型
            {
                for (int i = 0; i < data_qty; i++)
                {
                    uint32_t read_data = data[26 + i * 4] << 24 | data[27 + i * 4] << 16 | data[28 + i * 4] << 8 | data[29 + i * 4];
                    LOG_D("READ: dword, %d; com: %d", read_data, com);
                }
            }
            else
                LOG_W("READ: unknown data type: Not T/C/HC |OR| Flag not match");
        }
        else
        {
            LOG_W("READ: unknown data type: 0x%02X; com: %d", data[19], com);
            //LOG_HEX("READ response data", 32, data, len);
        }
    }
    else if (rw_type == PPI_WRITE)// 写数据，res2中看不到数据所以只能取req1的数据
    {
        uint16_t data_qty = data[23] << 8 | data[24];
        if (ppi_ctx->req_ctx[com - 1].data_type == BIT)
        {
            uint8_t write_data = data[35] & 0x01;
            //LOG_I("WRITE: bit, %d; com: %d", write_data, com);
        }
        else if (ppi_ctx->req_ctx[com - 1].data_type == BYTE)
        {
            //LOG_I("WRITE: byte, qty=%d ; com: %d; value=", data_qty, com);
            for (int i = 0; i < data_qty; i++)
            {
                uint8_t write_data = data[35 + i];
                //LOG_I("0x%02X ", write_data);
            }
        }
        else if (ppi_ctx->req_ctx[com - 1].data_type == WORD)
        {
            for (int i = 0; i < data_qty; i++)
            {
                uint16_t write_data = data[35 + i * 2] << 8 | data[36 + i * 2];
                //LOG_I("WRITE: word, %d; com: %d", write_data, com);
            }
        }
        else if (ppi_ctx->req_ctx[com - 1].data_type == DWORD)
        {
            for (int i = 0; i < data_qty; i++)
            {
                uint32_t write_data = data[35 + i * 4] << 24 | data[36 + i * 4] << 16 | data[37 + i * 4] << 8 | data[38 + i * 4];
                //LOG_I("WRITE: dword, %d; com: %d", write_data, com);
            }
        }
        else if (ppi_ctx->req_ctx[com - 1].data_type == T)
        {
            for (int i = 0; i < 5; i++)
            {
                uint16_t write_data = data[35 + i];
                //LOG_I("WRITE: T, %d; com: %d", write_data, com);
            }
        }
        else if (ppi_ctx->req_ctx[com - 1].data_type == C)
        {
            for (int i = 0; i < 3; i++)
            {
                uint16_t write_data = data[35 + i];
                //LOG_I("WRITE: C, %d; com: %d", write_data, com);
            }
        }
        else
            LOG_W("WRITE: unknown data type: 0x%02X; com: %d", data[19], com);
    }
    else
    {
        LOG_W("Unknown PPI function neither read nor write: 0x%02X", data[19]);
    }
}

// 获取变量类型
static int get_ppi_data_type(uint8_t data_type)
{
    switch (data_type)
    {
    case 0x01:
        return BIT;//获取req1变量类型
    case 0x02:
        return BYTE;
    case 0x03:
        return WORD;
    case 0x04:
        return DWORD;
    case 0x1E:
        return C;// T,C均为Word类型，
    case 0x1F:
        return T;
    case 0x20:
        return HC;// HC为Dword类型
    default:
        return -1;
    }
}

static void on_pkt(void *ctx, int com, const uint8_t *data, uint32_t len, uint16_t txn_id)
{
    ppi_ctx_t *ppi_ctx = (ppi_ctx_t *)ctx;
    LOG_D("on_pkt: %d, %d", com, len);

    if (protocol_get_cfg()->sys_work_mode == WORK_MODE_LISTEN)
    {
        // 监听模式
        protocol_on_read(0, com, data, len);
        return;
    }

    protocol_on_read(txn_id, com, data, len);
    if (com == COM_PLC)
    {
        protocol_req_t *src = protocol_get_current_request();
        if (src == NULL)
            return;
        uint8_t current_com = src->com;// 获取当前PLC通信消息的来源com

        if (ppi_ctx->req_ctx[current_com - 1].protocol_type == PPI)// 该com采用PPI协议通信
        {
            switch (ppi_ctx->state)
            {
            case PPI_STATE_WAIT_ACK:// 处理res1
                ppi_ctx->state = PPI_STATE_WAIT_SECOND_REQ;
                protocol_on_partial_response(data, len);
                break;

            case PPI_STATE_WAIT_FINAL_RESP:// 处理res2
                if (data[19] == 0x04)
                    get_ppi_value(ctx, len, com, data, PPI_READ);// 获取读取命令的返回值

                ppi_ctx->state = PPI_STATE_IDLE;
                protocol_on_response(data, len);
                break;

            default:
                LOG_E("In Protocol PPI, Invalid PPI state %d", ppi_ctx->state);
                return;
            }
        }
        else if (ppi_ctx->req_ctx[current_com - 1].protocol_type == S7)// 该com采用S7协议通信
        {
            if (ppi_ctx->req_ctx[current_com - 1].s7_state == S7_TO_PLC_STATE_WAIT_ACK)// 等待PLC响应1
            {
                if (data[0] == 0xE5)
                {
                    rt_thread_mdelay(2);// 经测验这里需要延时，因为是自己手动构造的req2，免得发太快PLC响应不过来
                    uint8_t response[6] = { 0x10, 0x02, 0x00, 0x5C, 0x5E, 0x16 };//直接构造响应
                    ppi_ctx->req_ctx[current_com - 1].s7_state = S7_TO_PLC_STATE_WAIT_RESP;
                    protocol_add_partial_request(current_com, response, sizeof(response));
                }
                else
                    LOG_W("In Protocol S7, Invalid PLC RES 1: %d", data[0]);
            }
            else if (ppi_ctx->req_ctx[current_com - 1].s7_state == S7_TO_PLC_STATE_WAIT_RESP)// 等待PLC响应2
            {
                if (data[21] != 0xFF)// 响应2有错误,需要构造错误响应
                {
                    LOG_E("In Protocol S7, PLC response2 with error");
                    //LOG_HEX("Received RES2", len, data, len);
                    uint8_t unit_ref = ppi_ctx->req_ctx[current_com - 1].unit_ref;
                    if (data[19] == 0x04)
                    {
                        uint8_t e_data[25] = { 0 };
                        build_s7_error_response(data, len, unit_ref, e_data, data[19]);
                        //LOG_HEX("BUILD ERROR S7 RES (Read Error)", sizeof(e_data), e_data, sizeof(e_data));
                        protocol_on_response(e_data, sizeof(e_data));
                    }
                    else if (data[19] == 0x05)
                    {
                        uint8_t e_data[22] = { 0 };
                        build_s7_error_response(data, len, unit_ref, e_data, data[19]);
                        //LOG_HEX("BUILD ERROR S7 RES (Write Error)", sizeof(e_data), e_data, sizeof(e_data));
                        protocol_on_response(e_data, sizeof(e_data));
                    }
                    else
                        LOG_W("In Protocol S7, Unknown function code in Building ERROR S7 RES: 0x%02X", data[19]);
                    return;
                }

                if (data[19] == 0x04)// 读响应处理, 把res2转为S7响应
                {
                    uint8_t s7_data[25 + (data[16] - 4)];//存ppi转成的s7数据
                    //LOG_HEX("Received RES2", len, data, len);
                    convert_ppi_to_s7(data, len, s7_data, sizeof(s7_data), ppi_ctx->req_ctx[current_com - 1].unit_ref);
                    //LOG_HEX("CONVERTED RES2", sizeof(s7_data), s7_data, sizeof(s7_data));
                    protocol_on_response(s7_data, sizeof(s7_data));
                }
                else if (data[19] == 0x05)// 写响应处理, 把res2转为S7响应
                {
                    uint8_t s7_data[22];
                    //LOG_HEX("Received RES2", len, data, len);
                    convert_ppi_to_s7(data, len, s7_data, 22, ppi_ctx->req_ctx[current_com - 1].unit_ref);
                    //LOG_HEX("CONVERTED RES2", 22, s7_data, 22);
                    protocol_on_response(s7_data, 22);
                }
                else// force unforce没弄
                    LOG_W("In Protocol S7, Unknown S7 function code in RES 2: 0x%02X", data[19]);

                ppi_ctx->req_ctx[current_com - 1].s7_state = S7_STATE_CONFIRMED;//收到了最后的响应，该次请求结束，重置状态
            }
        }
        else
            LOG_W("Invalid PLC protocol: %d", ppi_ctx->req_ctx[current_com - 1].protocol_type);
    }
    else // 非PLC数据
    {
        if (ppi_ctx->req_ctx[com - 1].protocol_type == PPI)
        {
            switch (ppi_ctx->state)
            {
            case PPI_STATE_IDLE:// 处理req1
                get_ppi_addr(ctx, len, data);//获取地址

                if (get_ppi_data_type(data[22]) != -1)//获取变量类型
                    ppi_ctx->req_ctx[com - 1].data_type = get_ppi_data_type(data[22]);
                else
                    LOG_W("In Protocol PPI, Unknown data type in REQ 1: 0x%02X", data[22]);

                if (data[17] == 0x04)//获取req1的读写类型
                    ppi_ctx->req_ctx[com - 1].rw_type = PPI_READ;
                else if (data[17] == 0x05)
                    ppi_ctx->req_ctx[com - 1].rw_type = PPI_WRITE;
                else if (data[17] == 0x00)//force unforce还没实现
                    ppi_ctx->req_ctx[com - 1].rw_type = PPI_FORCE_UNFORCE;
                else
                    LOG_W("In Protocol PPI, Unknown PPI function in REQ 1: 0x%02X", data[17]);

                if (ppi_ctx->req_ctx[com - 1].rw_type == PPI_WRITE)
                    get_ppi_value(ctx, len, com, data, PPI_WRITE); // 获取写入命令的值,目前只是打印

                ppi_ctx->state = PPI_STATE_WAIT_ACK;

                protocol_add_request(txn_id, com, 100, 0, data, len);
                break;

            case PPI_STATE_WAIT_SECOND_REQ:// 处理req2
                ppi_ctx->state = PPI_STATE_WAIT_FINAL_RESP;
                protocol_add_partial_request(com, data, len);
                break;

            default:
                LOG_E("BUG: Invalid state %d for HMI data", ppi_ctx->state);
                return;
            }
        }
        else if (ppi_ctx->req_ctx[com - 1].protocol_type == S7)
        {
            if (ppi_ctx->req_ctx[com - 1].s7_state < S7_STATE_CONFIRMED)// 握手环节直接转发构造好的报文
            {
                protocol_ppi_on_write(ctx, txn_id, com, data, len);
                ppi_ctx->req_ctx[com - 1].s7_state++;
            }
            else if (ppi_ctx->req_ctx[com - 1].s7_state == S7_STATE_CONFIRMED) // 收到读写报文
            {
                uint16_t temp_len = data[24];
                ppi_ctx->req_ctx[com - 1].unit_ref = data[11] << 8 | data[12];// 存储unit reference，构造响应的时候要用
                ppi_ctx->req_ctx[com - 1].s7_state = S7_TO_PLC_STATE_WAIT_ACK;

                if (data[17] == 0x04)
                {
                    uint8_t ppi_data[33];
                    on_parse_s7(data, len, ppi_data);
                    protocol_add_request(txn_id, com, 100, 0, ppi_data, sizeof(ppi_data));
                }
                else if (data[17] == 0x05)
                {
                    uint8_t ppi_data[35 + temp_len + 2];
                    on_parse_s7(data, len, ppi_data);
                    protocol_add_request(txn_id, com, 100, 0, ppi_data, sizeof(ppi_data));
                }
                else
                {
                    LOG_W("In Protocol S7, Unknown S7 function in REQ: 0x%02X", data[17]);
                }
            }
        }
        else
            LOG_W("Unknown protocol type: 0x%02X", ppi_ctx->req_ctx[com - 1].protocol_type);
    }
}

// 接收数据处理函数，找到完整的报文给on_pkt
int protocol_ppi_on_received(void *ctx, int com, const uint8_t *data, uint32_t len)
{
    ppi_ctx_t *ppi_ctx = (ppi_ctx_t *)ctx;
    LOG_D("protocol_ppi_on_received: %d, %d", com, len);
    //LOG_HEX("Received DATA", 32, data, len);

    uint16_t data_len = 0;// 存的是DA-DU末的长度，总报文长度还要加6
    uint16_t offset = 0;// 如res返回的是乱码+完整报文，记录无用报文长度
    bool complete_data = false;// 是否找到完整报文
    uint16_t txn_id = 0;
    int res = 0;// 存储报文校验结果，如果找到了完整报文就返回到报文末位的长度，反之返回要丢弃的数量

    if (com >= COM_HMI)// 非plc数据
    {
        if (IS_NET_COM(com) && (ppi_ctx->req_ctx[com - 1].protocol_type == S7 ||
            ppi_ctx->req_ctx[com - 1].protocol_type == UNDEFINE))//网口的消息，先判断是不是用S7协议
        {
            if (ppi_ctx->req_ctx[com - 1].s7_state == S7_STATE_IDLE) // s7第一次的握手
            {
                res = validate_s7_connect(data, len, &complete_data, &offset);
                if (complete_data)
                {
                    ppi_ctx->req_ctx[com - 1].protocol_type = S7; // 收到了S7的握手报文，直接设置该com采用S7协议
                    uint8_t *res_data = (uint8_t *)malloc(22);
                    build_s7_connect_res(data, len, offset, res_data, S7_STATE_IDLE);
                    txn_id = protocol_new_transaction();
                    on_pkt(ppi_ctx, com, res_data, 22, txn_id);
                    return res;
                }
            }
            else if (ppi_ctx->req_ctx[com - 1].s7_state == S7_STATE_WAIT_ACK)// s7第二次的握手
            {
                res = validate_s7_connect(data, len, &complete_data, &offset);
                if (complete_data)
                {
                    uint8_t *res_data = (uint8_t *)malloc(27);
                    build_s7_connect_res(data, len, offset, res_data, S7_STATE_WAIT_ACK);
                    txn_id = protocol_get_current_transaction();
                    on_pkt(ppi_ctx, com, res_data, 27, txn_id);
                }
                return res;
            }
            else if (ppi_ctx->req_ctx[com - 1].s7_state == S7_STATE_CONFIRMED)// confirm确认连接，处理s7协议读写请求
            {
                res = validate_s7_req(data, len, &complete_data, &offset, &data_len);
                if (complete_data)
                {
                    txn_id = protocol_get_current_transaction();
                    on_pkt(ppi_ctx, com, data + offset, data_len, txn_id);
                }
                return res;
            }
        }

        if (ppi_ctx->req_ctx[com - 1].protocol_type == PPI ||
            ppi_ctx->req_ctx[com - 1].protocol_type == UNDEFINE)// 判断是不是用PPI协议
        {
            switch (ppi_ctx->state)
            {
            case PPI_STATE_IDLE://空闲状态
                res = validate_ppi_req1(data, len, &complete_data, &data_len, &offset);
                if (complete_data)
                {
                    txn_id = protocol_new_transaction();//新的请求对应一个新的事务ID
                    if (data_len + 6 > len)
                    {
                        LOG_E("PPI frame too large: %d bytes", data_len + 6);
                        return offset; // 丢弃无效帧
                    }
                    ppi_ctx->req_ctx[com - 1].protocol_type = PPI;// 设置该com采用PPI协议
                    on_pkt(ppi_ctx, com, data + offset, data_len + 6, txn_id);
                }
                return res;

            case PPI_STATE_WAIT_SECOND_REQ://等待HMI的第二次请求,固定长度为6
                txn_id = protocol_get_current_transaction();
                res = validate_ppi_req2(data, len, &complete_data, &offset);
                if (complete_data)
                    on_pkt(ppi_ctx, com, data + offset, 6, txn_id);
                return res;

            default://端口与预期不符合
                LOG_W("(com>=COM_HMI) Unexpected state %d on com %d, discarding %d bytes", ppi_ctx->state, com, len);
                //LOG_HEX("Discarding DATA", 32, data, len);
                return len;
            }
        }
    }
    else
    {
        protocol_req_t *src = protocol_get_current_request();
        if (src == NULL)
            return len;
        uint8_t current_com = src->com;

        if (ppi_ctx->req_ctx[current_com - 1].protocol_type == PPI)
        {
            switch (ppi_ctx->state)
            {
            case PPI_STATE_WAIT_ACK://等待plc的ACK
                txn_id = protocol_get_current_transaction();//获取当前事务ID
                for (int i = 0; i < len; i++)
                {
                    if (data[i] == 0xE5)// 找到E5
                    {
                        on_pkt(ppi_ctx, com, data + i, 1, txn_id);
                        return i + 1;//当前的E5和之前的报文
                    }
                }
                return len;//没有E5，全是无用报文

            case PPI_STATE_WAIT_FINAL_RESP:// 等待PLC的res2
                txn_id = protocol_get_current_transaction();
                res = validate_ppi_res2(data, len, &complete_data, &data_len, &offset);
                if (complete_data)
                {
                    if (data_len + 6 > len)
                    {
                        LOG_E("PPI frame too large: %d bytes", data_len + 6);
                        return offset; // 丢弃无效帧
                    }
                    on_pkt(ppi_ctx, com, data + offset, data_len + 6, txn_id);
                }
                return res;

            default:
                LOG_W("(com==COM_PLC)Unexpected state %d on com %d, discarding %d bytes", ppi_ctx->state, com, len);
                //LOG_HEX("Discarding DATA", 32, data, len);
                return len;
            }
        }
        else if (ppi_ctx->req_ctx[current_com - 1].protocol_type == S7)
        {
            if (ppi_ctx->req_ctx[current_com - 1].s7_state == S7_TO_PLC_STATE_WAIT_ACK)//同PPI的处理，找E5
            {
                txn_id = protocol_get_current_transaction();//获取当前事务ID
                for (int i = 0; i < len; i++)
                {
                    if (data[i] == 0xE5)// 找到E5
                    {
                        on_pkt(ppi_ctx, com, data + i, 1, txn_id);
                        return i + 1;
                    }
                }
                return len;
            }
            else if (ppi_ctx->req_ctx[current_com - 1].s7_state == S7_TO_PLC_STATE_WAIT_RESP)//同PPI的接收res2
            {
                txn_id = protocol_get_current_transaction();
                res = validate_ppi_res2(data, len, &complete_data, &data_len, &offset);
                if (complete_data)
                {
                    if (data_len + 6 > len)
                    {
                        LOG_E("PPI frame too large: %d bytes", data_len + 6);
                        return offset; // 丢弃无效帧
                    }
                    on_pkt(ppi_ctx, com, data + offset, data_len + 6, txn_id);
                }
                return res;
            }
            else
            {
                LOG_W("In Protocol S7, Unexpected state %d on com %d, discarding %d bytes", ppi_ctx->state, com, len);
                //LOG_HEX("Discarding DATA", 32, data, len);
                return len;
            }
        }
    }
}

int protocol_ppi_on_write(void *ctx, uint32_t txn_id, int com, const uint8_t *data, uint32_t len)
{
    //LOG_I("protocol_ppi_on_write: %d, %d, txn_id=%u", com, len, txn_id);
    //LOG_HEX("DATA", 32, data, len);
    rt_thread_mdelay(2); //防止PLC收不到消息，1ms不行
    return protocol_on_write_base(txn_id, com, data, len);
}

void protocol_ppi_on_timeout(void *ctx, uint32_t txn_id, int com, const uint8_t *data, uint32_t len)
{
    ppi_ctx_t *ppi_ctx = (ppi_ctx_t *)ctx;
    LOG_W("protocol_ppi_on_timeout: %d, %u", com, txn_id);
    ppi_ctx->state = PPI_STATE_IDLE;
}

protocol_ops_t protocol_ppi_ops = {
    .init = protocol_ppi_init,
    .deinit = protocol_ppi_deinit,
    .on_connected = protocol_ppi_on_connected,
    .on_disconnected = protocol_ppi_on_disconnected,
    .on_received = protocol_ppi_on_received,
    .on_write = protocol_ppi_on_write,
    .on_timeout = protocol_ppi_on_timeout,
};
