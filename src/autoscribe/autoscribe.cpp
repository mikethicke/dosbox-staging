// SPDX-FileCopyrightText:  2026 Autoscribe contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "autoscribe.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "dosbox.h"
#include "gui/render/render.h"
#include "hardware/input/keyboard.h"
#include "hardware/input/mouse.h"
#include "hardware/memory.h"
#include "hardware/timer.h"
#include "hardware/video/vga.h"
#include "ints/int10.h"
#include "misc/rendered_image.h"
#include "misc/support.h"
#include "misc/video.h"

// ---------------------------------------------------------------------------
// Wire protocol (protocol/PROTOCOL.md, draft 0)
// ---------------------------------------------------------------------------

namespace {

constexpr uint16_t ProtocolVersion = 0;

enum class Msg : uint16_t {
	Hello        = 0x0001,
	Frame        = 0x0002,
	MemData      = 0x0003,
	MachineState = 0x0004,
	Goodbye      = 0x0005,

	MemRead     = 0x0101,
	MemWatch    = 0x0102,
	MemUnwatch  = 0x0103,
	Key         = 0x0104,
	MouseMove   = 0x0105,
	MouseButton = 0x0106,
	Pause       = 0x0107,
	Shutdown    = 0x0108,
};

constexpr uint16_t FlagAbsolute = 0x0001;

#pragma pack(push, 1)
struct Header {
	uint32_t length; // payload bytes
	uint16_t type;
	uint16_t flags;
};

struct HelloPayload {
	uint16_t proto_version;
	uint16_t pad;
	char shm_name[64];
	uint32_t shm_size;
};

struct FramePayload {
	uint8_t buffer;
	uint8_t pad[3];
	uint16_t width;
	uint16_t height;
	uint32_t stride;
	uint64_t frame_no;
	uint8_t is_text_mode;
};

struct MemReadPayload {
	uint32_t request_id;
	uint32_t address;
	uint32_t length;
};

struct MemDataHeader {
	uint32_t request_id;
	uint32_t address;
	uint32_t length;
};
#pragma pack(pop)

// Shared memory layout: two slots, each big enough for the largest frame
// DOSBox Staging renders plus a mirrored text buffer after the pixels.
constexpr size_t MaxFrameWidth  = 2048;
constexpr size_t MaxFrameHeight = 1536;
constexpr size_t MaxTextBytes   = 256 * 256 * 2;
constexpr size_t SlotBytes = MaxFrameWidth * MaxFrameHeight * 4 + MaxTextBytes;
constexpr size_t ShmBytes  = SlotBytes * 2;

constexpr uint32_t MaxMemRead = 1 << 20; // 1 MiB per request

struct Watch {
	uint32_t address = 0;
	uint32_t length  = 0;
	std::vector<uint8_t> last = {};
};

struct State {
	bool active       = false;
	int listen_fd     = -1;
	int client_fd     = -1;
	std::string socket_path = {};

	std::string shm_name = {};
	int shm_fd           = -1;
	uint8_t* shm         = nullptr;
	uint8_t next_slot    = 0;
	uint64_t frame_no    = 0;

	std::vector<uint8_t> rx = {};
	std::unordered_map<uint32_t, Watch> watches = {};
};

State state = {};

// ---------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------

void drop_client(const char* why)
{
	if (state.client_fd >= 0) {
		LOG_MSG("AUTOSCRIBE: Client disconnected (%s)", why);
		close(state.client_fd);
		state.client_fd = -1;
	}
	state.rx.clear();
	state.watches.clear();
}

// Writes everything or drops the client. Waits up to `timeout_ms` for the
// socket to drain so a briefly slow client doesn't get disconnected, but a
// stalled one can't block emulation for long.
bool send_all(const void* data, size_t len, const int timeout_ms = 250)
{
	if (state.client_fd < 0) {
		return false;
	}
	const auto* p = static_cast<const uint8_t*>(data);
	while (len > 0) {
		const auto n = send(state.client_fd, p, len, MSG_NOSIGNAL);
		if (n > 0) {
			p += n;
			len -= static_cast<size_t>(n);
			continue;
		}
		if (n < 0 && (errno == EAGAIN)) {
			pollfd pfd = {state.client_fd, POLLOUT, 0};
			if (poll(&pfd, 1, timeout_ms) > 0) {
				continue;
			}
			drop_client("send timeout");
			return false;
		}
		if (n < 0 && errno == EINTR) {
			continue;
		}
		drop_client("send failed");
		return false;
	}
	return true;
}

bool send_message(const Msg type, const void* payload, const uint32_t len,
                  const uint16_t flags = 0)
{
	const Header h = {len, static_cast<uint16_t>(type), flags};
	if (!send_all(&h, sizeof(h))) {
		return false;
	}
	return len == 0 || send_all(payload, len);
}

void send_hello()
{
	HelloPayload hello = {};
	hello.proto_version = ProtocolVersion;
	safe_strcpy(hello.shm_name, state.shm_name.c_str());
	hello.shm_size = static_cast<uint32_t>(ShmBytes);
	send_message(Msg::Hello, &hello, sizeof(hello));
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------

uint32_t memory_bytes()
{
	return MEM_TotalPages() * 4096u;
}

void read_memory(const uint32_t address, const uint32_t length,
                 std::vector<uint8_t>& out)
{
	out.resize(length);
	const auto total = memory_bytes();
	for (uint32_t i = 0; i < length; ++i) {
		const auto a = address + i;
		out[i] = (a < total) ? mem_readb(a) : 0;
	}
}

void send_mem_data(const uint32_t request_id, const uint32_t address,
                   const std::vector<uint8_t>& bytes)
{
	const MemDataHeader h = {request_id,
	                         address,
	                         static_cast<uint32_t>(bytes.size())};
	const Header wire = {static_cast<uint32_t>(sizeof(h) + bytes.size()),
	                     static_cast<uint16_t>(Msg::MemData),
	                     0};
	if (!send_all(&wire, sizeof(wire))) {
		return;
	}
	if (!send_all(&h, sizeof(h))) {
		return;
	}
	send_all(bytes.data(), bytes.size());
}

void poll_watches()
{
	static std::vector<uint8_t> buf;
	for (auto& [id, w] : state.watches) {
		read_memory(w.address, w.length, buf);
		if (buf != w.last) {
			w.last = buf;
			send_mem_data(id, w.address, buf);
		}
	}
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// DOS scancode set 1 -> KBD_KEYS. The high bit marks an E0-prefixed
// (extended) key, mirroring the convention in the Autoscribe profiles.
KBD_KEYS key_for_scancode(const uint8_t code)
{
	static const std::unordered_map<uint8_t, KBD_KEYS> base = {
	        {0x01, KBD_esc},        {0x02, KBD_1},          {0x03, KBD_2},
	        {0x04, KBD_3},          {0x05, KBD_4},          {0x06, KBD_5},
	        {0x07, KBD_6},          {0x08, KBD_7},          {0x09, KBD_8},
	        {0x0A, KBD_9},          {0x0B, KBD_0},          {0x0C, KBD_minus},
	        {0x0D, KBD_equals},     {0x0E, KBD_backspace},  {0x0F, KBD_tab},
	        {0x10, KBD_q},          {0x11, KBD_w},          {0x12, KBD_e},
	        {0x13, KBD_r},          {0x14, KBD_t},          {0x15, KBD_y},
	        {0x16, KBD_u},          {0x17, KBD_i},          {0x18, KBD_o},
	        {0x19, KBD_p},          {0x1A, KBD_leftbracket}, {0x1B, KBD_rightbracket},
	        {0x1C, KBD_enter},      {0x1D, KBD_leftctrl},   {0x1E, KBD_a},
	        {0x1F, KBD_s},          {0x20, KBD_d},          {0x21, KBD_f},
	        {0x22, KBD_g},          {0x23, KBD_h},          {0x24, KBD_j},
	        {0x25, KBD_k},          {0x26, KBD_l},          {0x27, KBD_semicolon},
	        {0x28, KBD_quote},      {0x29, KBD_grave},      {0x2A, KBD_leftshift},
	        {0x2B, KBD_backslash},  {0x2C, KBD_z},          {0x2D, KBD_x},
	        {0x2E, KBD_c},          {0x2F, KBD_v},          {0x30, KBD_b},
	        {0x31, KBD_n},          {0x32, KBD_m},          {0x33, KBD_comma},
	        {0x34, KBD_period},     {0x35, KBD_slash},      {0x36, KBD_rightshift},
	        {0x37, KBD_kpmultiply}, {0x38, KBD_leftalt},    {0x39, KBD_space},
	        {0x3A, KBD_capslock},   {0x3B, KBD_f1},         {0x3C, KBD_f2},
	        {0x3D, KBD_f3},         {0x3E, KBD_f4},         {0x3F, KBD_f5},
	        {0x40, KBD_f6},         {0x41, KBD_f7},         {0x42, KBD_f8},
	        {0x43, KBD_f9},         {0x44, KBD_f10},        {0x45, KBD_numlock},
	        {0x46, KBD_scrolllock}, {0x47, KBD_kp7},        {0x48, KBD_kp8},
	        {0x49, KBD_kp9},        {0x4A, KBD_kpminus},    {0x4B, KBD_kp4},
	        {0x4C, KBD_kp5},        {0x4D, KBD_kp6},        {0x4E, KBD_kpplus},
	        {0x4F, KBD_kp1},        {0x50, KBD_kp2},        {0x51, KBD_kp3},
	        {0x52, KBD_kp0},        {0x53, KBD_kpperiod},   {0x57, KBD_f11},
	        {0x58, KBD_f12},
	};
	static const std::unordered_map<uint8_t, KBD_KEYS> extended = {
	        {0x1C, KBD_kpenter},  {0x1D, KBD_rightctrl}, {0x35, KBD_kpdivide},
	        {0x38, KBD_rightalt}, {0x47, KBD_home},      {0x48, KBD_up},
	        {0x49, KBD_pageup},   {0x4B, KBD_left},      {0x4D, KBD_right},
	        {0x4F, KBD_end},      {0x50, KBD_down},      {0x51, KBD_pagedown},
	        {0x52, KBD_insert},   {0x53, KBD_delete},
	};
	const auto& table = (code & 0x80) ? extended : base;
	const auto it     = table.find(code & 0x7F);
	return it == table.end() ? KBD_NONE : it->second;
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------

template <typename T>
bool payload_as(const std::vector<uint8_t>& rx, const size_t offset,
                const uint32_t len, T& out)
{
	if (len < sizeof(T)) {
		return false;
	}
	std::memcpy(&out, rx.data() + offset, sizeof(T));
	return true;
}

void handle_message(const Header& h, const size_t payload_offset)
{
	const auto& rx = state.rx;
	switch (static_cast<Msg>(h.type)) {
	case Msg::MemRead: {
		MemReadPayload p = {};
		if (!payload_as(rx, payload_offset, h.length, p)) {
			return;
		}
		std::vector<uint8_t> bytes;
		read_memory(p.address, std::min(p.length, MaxMemRead), bytes);
		send_mem_data(p.request_id, p.address, bytes);
		break;
	}
	case Msg::MemWatch: {
		MemReadPayload p = {};
		if (!payload_as(rx, payload_offset, h.length, p)) {
			return;
		}
		Watch w;
		w.address = p.address;
		w.length  = std::min(p.length, MaxMemRead);
		// Send the initial value immediately so the client needn't wait
		// for a change.
		read_memory(w.address, w.length, w.last);
		send_mem_data(p.request_id, w.address, w.last);
		state.watches[p.request_id] = std::move(w);
		break;
	}
	case Msg::MemUnwatch: {
		uint32_t id = 0;
		if (payload_as(rx, payload_offset, h.length, id)) {
			state.watches.erase(id);
		}
		break;
	}
	case Msg::Key: {
		if (h.length < 2) {
			return;
		}
		const auto code    = rx[payload_offset];
		const bool pressed = rx[payload_offset + 1] != 0;
		const auto key     = key_for_scancode(code);
		if (key != KBD_NONE) {
			KEYBOARD_AddKey(key, pressed);
		}
		break;
	}
	case Msg::MouseMove: {
		if (h.length < 4) {
			return;
		}
		if (h.flags & FlagAbsolute) {
			// TODO: absolute positioning needs the render geometry;
			// relative motion is enough for the first target games.
			return;
		}
		int16_t dx = 0, dy = 0;
		std::memcpy(&dx, rx.data() + payload_offset, 2);
		std::memcpy(&dy, rx.data() + payload_offset + 2, 2);
		MOUSE_EventMoved(static_cast<float>(dx),
		                 static_cast<float>(dy),
		                 0.0f,
		                 0.0f);
		break;
	}
	case Msg::MouseButton: {
		if (h.length < 2) {
			return;
		}
		const auto button  = rx[payload_offset];
		const bool pressed = rx[payload_offset + 1] != 0;
		if (button <= static_cast<uint8_t>(MouseButtonId::Extra2)) {
			MOUSE_EventButton(static_cast<MouseButtonId>(button), pressed);
		}
		break;
	}
	case Msg::Pause:
		// TODO: hook the pause state machine.
		break;
	case Msg::Shutdown: {
		SDL_Event e = {};
		e.type      = SDL_EVENT_QUIT;
		SDL_PushEvent(&e);
		break;
	}
	default:
		LOG_WARNING("AUTOSCRIBE: Unknown message type 0x%04x", h.type);
		break;
	}
}

void read_client()
{
	uint8_t buf[65536];
	for (;;) {
		const auto n = recv(state.client_fd, buf, sizeof(buf), 0);
		if (n > 0) {
			state.rx.insert(state.rx.end(), buf, buf + n);
			continue;
		}
		if (n == 0) {
			drop_client("closed");
			return;
		}
		if (errno == EAGAIN) {
			break;
		}
		if (errno == EINTR) {
			continue;
		}
		drop_client("recv failed");
		return;
	}

	size_t offset = 0;
	while (state.rx.size() - offset >= sizeof(Header)) {
		Header h = {};
		std::memcpy(&h, state.rx.data() + offset, sizeof(h));
		if (h.length > (16u << 20)) {
			drop_client("oversized message");
			return;
		}
		if (state.rx.size() - offset < sizeof(Header) + h.length) {
			break;
		}
		handle_message(h, offset + sizeof(Header));
		if (state.client_fd < 0) {
			return;
		}
		offset += sizeof(Header) + h.length;
	}
	if (offset > 0) {
		state.rx.erase(state.rx.begin(),
		               state.rx.begin() + static_cast<long>(offset));
	}
}

void tick()
{
	if (!state.active) {
		return;
	}
	if (state.client_fd < 0) {
		const auto fd = accept(state.listen_fd, nullptr, nullptr);
		if (fd < 0) {
			return;
		}
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
		state.client_fd = fd;
		state.rx.clear();
		LOG_MSG("AUTOSCRIBE: Client connected");
		send_hello();
		return;
	}
	read_client();
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

// Converts the renderer's source image into RGBA8888 in the given slot.
void convert_frame(const RenderedImage& img, uint8_t* dst)
{
	const int w = img.params.width;
	const int h = img.params.height;
	const auto dst_stride = static_cast<size_t>(w) * 4;

	for (int y = 0; y < h; ++y) {
		const int src_y = img.is_flipped_vertically ? (h - 1 - y) : y;
		const uint8_t* src = img.image_data +
		                     static_cast<size_t>(src_y) * img.pitch;
		uint8_t* out = dst + static_cast<size_t>(y) * dst_stride;

		switch (img.params.pixel_format) {
		case PixelFormat::Indexed8:
			for (int x = 0; x < w; ++x) {
				const auto& c = img.palette[src[x]];
				out[x * 4 + 0] = c.red;
				out[x * 4 + 1] = c.green;
				out[x * 4 + 2] = c.blue;
				out[x * 4 + 3] = 255;
			}
			break;
		case PixelFormat::RGB555_Packed16:
			for (int x = 0; x < w; ++x) {
				uint16_t p = 0;
				std::memcpy(&p, src + x * 2, 2);
				const auto r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
				out[x * 4 + 0] = static_cast<uint8_t>((r << 3) | (r >> 2));
				out[x * 4 + 1] = static_cast<uint8_t>((g << 3) | (g >> 2));
				out[x * 4 + 2] = static_cast<uint8_t>((b << 3) | (b >> 2));
				out[x * 4 + 3] = 255;
			}
			break;
		case PixelFormat::RGB565_Packed16:
			for (int x = 0; x < w; ++x) {
				uint16_t p = 0;
				std::memcpy(&p, src + x * 2, 2);
				const auto r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
				out[x * 4 + 0] = static_cast<uint8_t>((r << 3) | (r >> 2));
				out[x * 4 + 1] = static_cast<uint8_t>((g << 2) | (g >> 4));
				out[x * 4 + 2] = static_cast<uint8_t>((b << 3) | (b >> 2));
				out[x * 4 + 3] = 255;
			}
			break;
		case PixelFormat::BGR24_ByteArray:
			for (int x = 0; x < w; ++x) {
				out[x * 4 + 0] = src[x * 3 + 2];
				out[x * 4 + 1] = src[x * 3 + 1];
				out[x * 4 + 2] = src[x * 3 + 0];
				out[x * 4 + 3] = 255;
			}
			break;
		case PixelFormat::BGRX32_ByteArray:
			for (int x = 0; x < w; ++x) {
				out[x * 4 + 0] = src[x * 4 + 2];
				out[x * 4 + 1] = src[x * 4 + 1];
				out[x * 4 + 2] = src[x * 4 + 0];
				out[x * 4 + 3] = 255;
			}
			break;
		}
	}
}

// Mirrors the active text page (char, attribute pairs) after the pixels.
// Returns the number of bytes written, 0 if not in a text mode.
uint32_t mirror_text_buffer(uint8_t* dst, uint16_t& cols, uint16_t& rows)
{
	cols = rows = 0;
	if (vga.mode != M_TEXT) {
		return 0;
	}
	cols = CurMode->twidth;
	rows = CurMode->theight;
	const uint32_t bytes = static_cast<uint32_t>(cols) * rows * 2;
	if (bytes == 0 || bytes > MaxTextBytes) {
		return 0;
	}
	const uint32_t page_start = real_readw(BIOSMEM_SEG, BIOSMEM_CURRENT_START);
	const uint32_t base = 0xB8000 + page_start;
	for (uint32_t i = 0; i < bytes; ++i) {
		dst[i] = mem_readb(base + i);
	}
	return bytes;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool AUTOSCRIBE_IsActive()
{
	return state.active;
}

void AUTOSCRIBE_Init(const std::string& socket_path)
{
	if (socket_path.empty()) {
		return;
	}

	// Shared memory for frames
	state.shm_name = "/autoscribe-" + std::to_string(getpid());
	shm_unlink(state.shm_name.c_str());
	state.shm_fd = shm_open(state.shm_name.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
	if (state.shm_fd < 0 || ftruncate(state.shm_fd, static_cast<off_t>(ShmBytes)) != 0) {
		LOG_ERR("AUTOSCRIBE: Cannot create shared memory '%s': %s",
		        state.shm_name.c_str(), strerror(errno));
		return;
	}
	state.shm = static_cast<uint8_t*>(mmap(nullptr, ShmBytes, PROT_READ | PROT_WRITE,
	                                       MAP_SHARED, state.shm_fd, 0));
	if (state.shm == MAP_FAILED) {
		LOG_ERR("AUTOSCRIBE: mmap failed: %s", strerror(errno));
		state.shm = nullptr;
		return;
	}

	// Listening socket
	unlink(socket_path.c_str());
	state.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	sockaddr_un addr = {};
	addr.sun_family  = AF_UNIX;
	if (socket_path.size() >= sizeof(addr.sun_path)) {
		LOG_ERR("AUTOSCRIBE: Socket path too long");
		return;
	}
	std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
	if (bind(state.listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
	    listen(state.listen_fd, 1) != 0) {
		LOG_ERR("AUTOSCRIBE: Cannot listen on '%s': %s",
		        socket_path.c_str(), strerror(errno));
		return;
	}
	state.socket_path = socket_path;
	state.active      = true;

	TIMER_AddTickHandler(tick);
	LOG_MSG("AUTOSCRIBE: Listening on '%s', frames in '%s'",
	        socket_path.c_str(), state.shm_name.c_str());
}

void AUTOSCRIBE_Shutdown()
{
	if (!state.active) {
		return;
	}
	if (state.client_fd >= 0) {
		send_message(Msg::Goodbye, nullptr, 0);
	}
	drop_client("shutdown");
	if (state.listen_fd >= 0) {
		close(state.listen_fd);
		state.listen_fd = -1;
	}
	unlink(state.socket_path.c_str());
	if (state.shm) {
		munmap(state.shm, ShmBytes);
		state.shm = nullptr;
	}
	if (state.shm_fd >= 0) {
		close(state.shm_fd);
		state.shm_fd = -1;
	}
	shm_unlink(state.shm_name.c_str());
	state.active = false;
}

void AUTOSCRIBE_OnFrame()
{
	if (!state.active || state.client_fd < 0) {
		return;
	}
	const auto img = RENDER_GetCurrentImage();
	if (!img.image_data) {
		return;
	}
	const int w = img.params.width;
	const int h = img.params.height;
	if (w <= 0 || h <= 0 || static_cast<size_t>(w) > MaxFrameWidth ||
	    static_cast<size_t>(h) > MaxFrameHeight) {
		return;
	}

	const auto slot = state.next_slot;
	state.next_slot = static_cast<uint8_t>(1 - slot);
	uint8_t* base   = state.shm + slot * SlotBytes;

	convert_frame(img, base);

	uint16_t cols = 0, rows = 0;
	const auto pixel_bytes = static_cast<size_t>(w) * h * 4;
	const auto text_bytes  = mirror_text_buffer(base + pixel_bytes, cols, rows);

	FramePayload f  = {};
	f.buffer        = slot;
	f.width         = static_cast<uint16_t>(w);
	f.height        = static_cast<uint16_t>(h);
	f.stride        = static_cast<uint32_t>(w) * 4;
	f.frame_no      = ++state.frame_no;
	f.is_text_mode  = text_bytes > 0 ? 1 : 0;
	// Text dimensions ride in the flags word: cols in the low byte, rows
	// in the high byte.
	const uint16_t flags = static_cast<uint16_t>(cols | (rows << 8));
	send_message(Msg::Frame, &f, sizeof(f), flags);

	poll_watches();
}
