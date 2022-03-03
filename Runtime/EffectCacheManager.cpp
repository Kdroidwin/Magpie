#include "pch.h"
#include "EffectCacheManager.h"
#include <yas/mem_streams.hpp>
#include <yas/binary_oarchive.hpp>
#include <yas/binary_iarchive.hpp>
#include <yas/types/std/pair.hpp>
#include <yas/types/std/string.hpp>
#include <yas/types/std/vector.hpp>
#include "EffectCompiler.h"
#include <regex>
#include "App.h"
#include "DeviceResources.h"
#include "StrUtils.h"
#include "Logger.h"
#include <zstd.h>


template<typename Archive>
void serialize(Archive& ar, winrt::com_ptr<ID3DBlob>& o) {
	SIZE_T size = 0;
	ar& size;
	HRESULT hr = D3DCreateBlob(size, o.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("D3DCreateBlob 失败", hr);
		throw new std::exception();
	}

	BYTE* buf = (BYTE*)o->GetBufferPointer();
	for (SIZE_T i = 0; i < size; ++i) {
		ar& (*buf++);
	}
}

template<typename Archive>
void serialize(Archive& ar, const winrt::com_ptr<ID3DBlob>& o) {
	SIZE_T size = o->GetBufferSize();
	ar& size;

	BYTE* buf = (BYTE*)o->GetBufferPointer();
	for (SIZE_T i = 0; i < size; ++i) {
		ar& (*buf++);
	}
}

template<typename Archive>
void serialize(Archive& ar, const EffectParameterDesc& o) {
	size_t index = o.defaultValue.index();
	ar& index;
	
	if (index == 0) {
		ar& std::get<0>(o.defaultValue);
	} else {
		ar& std::get<1>(o.defaultValue);
	}
	
	ar& o.label;

	index = o.maxValue.index();
	ar& index;
	if (index == 1) {
		ar& std::get<1>(o.maxValue);
	} else if (index == 2) {
		ar& std::get<2>(o.maxValue);
	}

	index = o.minValue.index();
	ar& index;
	if (index == 1) {
		ar& std::get<1>(o.minValue);
	} else if (index == 2) {
		ar& std::get<2>(o.minValue);
	}

	ar& o.name& o.type;
}

template<typename Archive>
void serialize(Archive& ar, EffectParameterDesc& o) {
	size_t index = 0;
	ar& index;

	if (index == 0) {
		o.defaultValue.emplace<0>();
		ar& std::get<0>(o.defaultValue);
	} else {
		o.defaultValue.emplace<1>();
		ar& std::get<1>(o.defaultValue);
	}

	ar& o.label;

	ar& index;
	if (index == 0) {
		o.maxValue.emplace<0>();
	} else if (index == 1) {
		o.maxValue.emplace<1>();
		ar& std::get<1>(o.maxValue);
	} else {
		o.maxValue.emplace<2>();
		ar& std::get<2>(o.maxValue);
	}

	ar& index;
	if (index == 0) {
		o.minValue.emplace<0>();
	} else if (index == 1) {
		o.minValue.emplace<1>();
		ar& std::get<1>(o.minValue);
	} else {
		o.minValue.emplace<2>();
		ar& std::get<2>(o.minValue);
	}

	ar& o.name& o.type;
}

template<typename Archive>
void serialize(Archive& ar, EffectIntermediateTextureDesc& o) {
	ar& o.format& o.name & o.source & o.sizeExpr;
}

template<typename Archive>
void serialize(Archive& ar, EffectSamplerDesc& o) {
	ar& o.filterType& o.addressType& o.name;
}

template<typename Archive>
void serialize(Archive& ar, EffectPassDesc& o) {
	ar& o.inputs& o.outputs& o.cso& o.blockSize.first& o.blockSize.second;
}

template<typename Archive>
void serialize(Archive& ar, EffectDesc& o) {
	ar& o.outSizeExpr& o.params& o.textures& o.samplers& o.passes;
}


std::wstring ConvertFileName(const wchar_t* fileName) {
	std::wstring file(fileName);

	// 删除文件名中的路径
	size_t pos = file.find_last_of('\\');
	if (pos != std::wstring::npos) {
		file.erase(0, pos + 1);
	}

	std::replace(file.begin(), file.end(), '.', '_');
	return file;
}

std::wstring EffectCacheManager::_GetCacheFileName(const wchar_t* fileName, std::string_view hash) {
	return fmt::format(L".\\cache\\{}_{}.{}", ConvertFileName(fileName), StrUtils::UTF8ToUTF16(hash), _SUFFIX);
}

void EffectCacheManager::_AddToMemCache(const std::wstring& cacheFileName, const EffectDesc& desc) {
	_memCache[cacheFileName] = desc;

	if (_memCache.size() > _MAX_CACHE_COUNT) {
		// 清理一半内存缓存
		auto it = _memCache.begin();
		std::advance(it, _memCache.size() / 2);
		_memCache.erase(_memCache.begin(), it);

		Logger::Get().Info("已清理内存缓存");
	}
}


bool EffectCacheManager::Load(const wchar_t* fileName, std::string_view hash, EffectDesc& desc) {
	if (App::Get().IsDisableEffectCache()) {
		return false;
	}

	std::wstring cacheFileName = _GetCacheFileName(fileName, hash);

	auto it = _memCache.find(cacheFileName);
	if (it != _memCache.end()) {
		desc = it->second;
		return true;
	}

	if (!Utils::FileExists(cacheFileName.c_str())) {
		return false;
	}
	
	std::vector<BYTE> buf;
	if (!Utils::ReadFile(cacheFileName.c_str(), buf) || buf.empty()) {
		return false;
	}

	if (buf.size() < 100) {
		return false;
	}
	
	// 格式：HASH-VERSION-FL-{BODY}

	// 检查哈希
	std::vector<BYTE> bufHash;
	if (!Utils::Hasher::Get().Hash(
		buf.data() + Utils::Hasher::Get().GetHashLength(),
		buf.size() - Utils::Hasher::Get().GetHashLength(),
		bufHash
	)) {
		Logger::Get().Error("计算哈希失败");
		return false;
	}

	if (std::memcmp(buf.data(), bufHash.data(), bufHash.size()) != 0) {
		Logger::Get().Error("缓存文件校验失败");
		return false;
	}

	try {
		yas::mem_istream mi(buf.data() + bufHash.size(), buf.size() - bufHash.size());
		yas::binary_iarchive<yas::mem_istream, yas::binary> ia(mi);

		// 检查版本
		UINT version;
		ia& version;
		if (version != _VERSION) {
			Logger::Get().Info("缓存版本不匹配");
			return false;
		}

		// 检查 Direct3D 功能级别
		D3D_FEATURE_LEVEL fl;
		ia& fl;
		if (fl != App::Get().GetDeviceResources().GetFeatureLevel()) {
			Logger::Get().Info("功能级别不匹配");
			return false;
		}


		ia& desc;
	} catch (...) {
		Logger::Get().Error("反序列化失败");
		desc = {};
		return false;
	}

	_AddToMemCache(cacheFileName, desc);
	
	Logger::Get().Info("已读取缓存 " + StrUtils::UTF16ToUTF8(cacheFileName));
	return true;
}

void EffectCacheManager::Save(const wchar_t* fileName, std::string_view hash, const EffectDesc& desc) {
	if (App::Get().IsDisableEffectCache()) {
		return;
	}

	// 格式：HASH-VERSION-FL-{BODY}

	std::vector<BYTE> buf;
	buf.reserve(4096);
	buf.resize(Utils::Hasher::Get().GetHashLength());

	try {
		yas::vector_ostream os(buf);
		yas::binary_oarchive<yas::vector_ostream<BYTE>, yas::binary> oa(os);

		oa& _VERSION;
		oa& App::Get().GetDeviceResources().GetFeatureLevel();
		oa& desc;
	} catch (...) {
		Logger::Get().Error("序列化失败");
		return;
	}

	// 填充 HASH
	std::vector<BYTE> bufHash;
	if (!Utils::Hasher::Get().Hash(
		buf.data() + Utils::Hasher::Get().GetHashLength(),
		buf.size() - Utils::Hasher::Get().GetHashLength(),
		bufHash
	)) {
		Logger::Get().Error("计算哈希失败");
		return;
	}
	std::memcpy(buf.data(), bufHash.data(), bufHash.size());


	if (!Utils::DirExists(L".\\cache")) {
		if (!CreateDirectory(L".\\cache", nullptr)) {
			Logger::Get().Win32Error("创建 cache 文件夹失败");
			return;
		}
	} else {
		// 删除所有该文件的缓存
		std::wregex regex(fmt::format(L"^{}_[0-9,a-f]{{{}}}.{}$", ConvertFileName(fileName),
				Utils::Hasher::Get().GetHashLength() * 2, _SUFFIX), std::wregex::optimize | std::wregex::nosubs);

		WIN32_FIND_DATA findData;
		HANDLE hFind = Utils::SafeHandle(FindFirstFile(L".\\cache\\*", &findData));
		if (hFind) {
			while (FindNextFile(hFind, &findData)) {
				if (StrUtils::StrLen(findData.cFileName) < 8) {
					continue;
				}

				// 正则匹配文件名
				if (!std::regex_match(findData.cFileName, regex)) {
					continue;
				}

				if (!DeleteFile((L".\\cache\\"s + findData.cFileName).c_str())) {
					Logger::Get().Win32Error(fmt::format("删除缓存文件 {} 失败",
						StrUtils::UTF16ToUTF8(findData.cFileName)));
				}
			}
			FindClose(hFind);
		} else {
			Logger::Get().Win32Error("查找缓存文件失败");
		}
	}
	
	std::wstring cacheFileName = _GetCacheFileName(fileName, hash);
	if (!Utils::WriteFile(cacheFileName.c_str(), buf.data(), buf.size())) {
		Logger::Get().Error("保存缓存失败");
	}

	_AddToMemCache(cacheFileName, desc);

	Logger::Get().Info("已保存缓存 " + StrUtils::UTF16ToUTF8(cacheFileName));
}
