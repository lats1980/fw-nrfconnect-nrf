#ifndef __USB_REQUEST_H__
#define __USB_REQUEST_H__

enum wifi_adapter_requests{
	RPU_ENABLE,
	IRQ_ENABLE,
	REGISTER_READ,
	REGISTER_WRITE,
	BLOCK_READ,
	BLOCK_WRITE
};

struct rpu_request {
	uint32_t cmd;
	union {
		struct {
			uint32_t addr;
		} read_reg;
		struct {
			uint32_t addr;
			uint32_t val;
		} write_reg;
		struct {
			uint32_t addr;
			int32_t count;
		} read_block;
		struct {
			uint32_t addr;
			int32_t count;
		} write_block;
	} __packed;
} __packed;

#endif /* __USB_REQUEST_H__ */
