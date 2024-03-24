#include <Windows.h>
#include <Psapi.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <print>
#include <string>
#include <thread>
#include <vector>

#include <Luau/BytecodeBuilder.h>
#include <Luau/BytecodeUtils.h>
#include <Luau/Compiler.h>
#include <Pattern16.h>
#include <safetyhook.hpp>
#include <xxhash.h>
#include <zstd.h>

const HMODULE module = GetModuleHandle(NULL);

void* operator new(size_t size) {
	return reinterpret_cast<void* (*)(size_t size)>(GetProcAddress(module, "rbxAllocate"))(size);
}

void operator delete(void* ptr) noexcept {
	reinterpret_cast<void (*)(void* ptr)>(GetProcAddress(module, "rbxDeallocate"))(ptr);
}

namespace types {
	typedef std::string(*compile)(const std::string& source, int target, int options);
	typedef void* (*deserialize_item)(void* self, void* result, void* in_bitstream, int item_type);
	enum item_type { client_qos = 0x1f };
	enum network_value_format { protected_string_bytecode = 0x2f };
}

namespace hooks {
	SafetyHookInline compile{};
	SafetyHookInline deserialize_item{};
}

types::compile compile;

std::string compile_hook(const std::string& source, int target, int options) {
	// TODO: figure out why blank scripts don't compile properly (they error in dev console, but not a big deal since they dont do anything anyway)
	
	// for some reason older studios (i believe feb 2023 and below) like to spam call LuaVM::compile with random args (that cause a crash) and i literally have no idea why
	if (target != 0) {
		return ""; // if the target isn't 0 don't compile it (prevent crash)
	}
	if (options != 8) {
		return ""; // if the options int isn't 8 don't compile it (prevent crash)
	}
	std::println(
		"LuaVM::compile(source: {}, target: {}, options: {})",
		"...", target, options
	);

	class bytecode_encoder_client : public Luau::BytecodeEncoder {
		void encode(uint32_t* data, size_t count) override {
			for (size_t i = 0; i < count;) {
				uint8_t op = LUAU_INSN_OP(data[i]);

				int oplen = Luau::getOpLength(LuauOpcode(op));
				uint8_t openc = uint8_t(op * 227);
				data[i] = openc | (data[i] & ~0xff);

				i += oplen;
			}
		}
	};

	bytecode_encoder_client encoder{};

	const char* special_globals[] = {
		"game",
		"Game",
		"workspace",
		"Workspace",
		"script",
		"shared",
		"plugin",
		nullptr
	};

	std::string bytecode = Luau::compile(source, { 2, 1, 0, "Vector3", "new", nullptr, special_globals }, {}, &encoder);

	size_t compressed_bytecode_capacity = ZSTD_compressBound(bytecode.length());
	uint32_t uncompressed_bytecode_size = static_cast<uint32_t>(bytecode.length());

	std::vector<std::uint8_t> encoded_bytecode(
		// Hash + Size + Compressed Bytecode
		4 + 4 + compressed_bytecode_capacity
	);

	std::memcpy(&encoded_bytecode[0], "RSB1", 4);
	std::memcpy(&encoded_bytecode[4], &uncompressed_bytecode_size, 4);

	size_t compressed_bytecode_size = ZSTD_compress(
		&encoded_bytecode[8],
		compressed_bytecode_capacity,
		bytecode.c_str(),
		bytecode.length(),
		ZSTD_maxCLevel()
	);

	size_t encoded_bytecode_size = 4 + 4 + compressed_bytecode_size;
	XXH32_hash_t hash = XXH32(encoded_bytecode.data(), encoded_bytecode_size, 42);
	std::uint8_t key[4]; std::memcpy(key, &hash, 4);

	for (size_t i = 0; i < encoded_bytecode_size; i++) {
		encoded_bytecode[i] ^= (key[i % 4]) + (i * 41);
	}

	return std::string(reinterpret_cast<char*>(encoded_bytecode.data()), encoded_bytecode_size);
}

types::deserialize_item deserialize_item;

void* deserialize_item_hook(void* self, void* deserialized_item, void* in_bitstream, int item_type) {
	std::println(
		"RBX::Network::Replicator::deserializeItem(this: {:p}, deserializedItem: {:p}, inBitstream: {:p}, itemType: {:x})",
		self, deserialized_item, in_bitstream, item_type
	);

	if (item_type == types::item_type::client_qos) {
		std::println("Bad qos");
		std::memset(deserialized_item, 0, 16);
		return deserialized_item;
	}

	return hooks::deserialize_item.call<void*>(self, deserialized_item, in_bitstream, item_type);
}

uintptr_t generate_schema_definition_packet;

void patch_generate_schema_definition_packet() {
	uintptr_t addr = generate_schema_definition_packet;
	std::byte byte = std::byte(types::network_value_format::protected_string_bytecode);

	DWORD old_protect;
	VirtualProtect(reinterpret_cast<void*>(addr), 16, PAGE_EXECUTE_READWRITE, &old_protect);
	*reinterpret_cast<std::byte*>(addr + 4) = byte;
	VirtualProtect(reinterpret_cast<void*>(addr), 16, old_protect, &old_protect);
}

void attach_console() {
	AllocConsole();
	static FILE* file; freopen_s(&file, "CONOUT$", "w", stdout);
	SetConsoleTitle(std::format("rsblox/local_rcc @ {}", __TIMESTAMP__).c_str());
}

void pattern_scan() {
	MODULEINFO info; GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info));
	void* start = reinterpret_cast<void*>(module); size_t size = info.SizeOfImage;

	compile = reinterpret_cast<types::compile>(Pattern16::scan(
		start, size, "33 C0 48 C7 41 18 0F 00 00 00 48 89 01 48 89 41 10 88 01 48 8B C1"
	));

	std::println(
		"Found compile @ {:p}",
		reinterpret_cast<void*>(compile)
	);

	/*
	old signature
	deserialize_item = reinterpret_cast<types::deserialize_item>(Pattern16::scan(
		start, size, "48 89 5C 24 ?? 48 89 74 24 ?? 48 89 54 24 ?? 55 57 41 56 48 8B EC 48 83 EC 40 49 8B F8"
	));
	*/
	deserialize_item = reinterpret_cast<types::deserialize_item>(Pattern16::scan(
		start, size, "48 89 5C 24 08 48 89 54 24 10 55 56 57 41 56 41 57 48 8D 6C 24 C9 48 81 EC C0000000 4D 8B F0"
	));

	std::println(
		"Found deserialize_item @ {:p}",
		reinterpret_cast<void*>(deserialize_item)
	);

	/*
	old signature
	generate_schema_definition_packet = reinterpret_cast<uintptr_t>(Pattern16::scan(
		start, size, "C6 44 24 ?? 06 EB ?? 48 3B 15"
	));
	*/
	generate_schema_definition_packet = reinterpret_cast<uintptr_t>(Pattern16::scan(
		start, size, "C6 44 24 ?? 06 EB ?? 48 8D 05"
	));

	std::println(
		"Found generate_schema_definition_packet @ {:p}",
		reinterpret_cast<void*>(generate_schema_definition_packet)
	);
}

void thread() {
	attach_console(); std::println("Hello, world!");
	pattern_scan(); std::println("Scanning finished.");

	hooks::compile = safetyhook::create_inline(compile, compile_hook);
	std::println("Hooked LuaVM::compile.");

	hooks::deserialize_item = safetyhook::create_inline(deserialize_item, deserialize_item_hook);
	std::println("Hooked RBX::Network::Replicator::deserializeItem.");

	patch_generate_schema_definition_packet();
	std::println("Patched RBX::Network::NetworkSchema::generateSchemaDefinitionPacket.");

	std::println("Made with <3 by 7ap & Epix @ https://github.com/rsblox - come join us!");
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
	DisableThreadLibraryCalls(module);

	if (reason == DLL_PROCESS_ATTACH) {
		std::thread(thread).detach();
	}

	return TRUE;
}
