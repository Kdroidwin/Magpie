// MPVHookTextureParser.cpp : ���ļ����� "main" ����������ִ�н��ڴ˴���ʼ��������
//

#include <iostream>
#include <fstream>
#include <vector>
#include <DirectXTex.h>
#include <DirectXPackedVector.h>
#include <string_view>


std::wstring UTF8ToUTF16(std::string_view str) {
	int convertResult = MultiByteToWideChar(CP_ACP, 0, str.data(), (int)str.size(), nullptr, 0);
	if (convertResult <= 0) {
		assert(false);
		return {};
	}

	std::wstring r(convertResult + 10, L'\0');
	convertResult = MultiByteToWideChar(CP_ACP, 0, str.data(), (int)str.size(), &r[0], (int)r.size());
	if (convertResult <= 0) {
		assert(false);
		return {};
	}

	return std::wstring(r.begin(), r.begin() + convertResult);
}

int main(int argc, char* argv[]) {
	if (argc != 3) {
		std::cout << "�Ƿ�����" << std::endl;
		return 1;
	}

	const char* inFile = argv[1];
	const char* outFile = argv[2];

	std::ifstream ifs(inFile);
	if (!ifs) {
		std::cout << "��" << inFile << "ʧ��" << std::endl;
		return 1;
	}

	size_t width, height;
	ifs >> width >> height;

	std::vector<DirectX::PackedVector::HALF> data(width * height * 4);
	for (size_t i = 0; i < data.size(); ++i) {
		float f;
		ifs >> f;
		data[i] = DirectX::PackedVector::XMConvertFloatToHalf(f);
	}

	DirectX::Image img{};
	img.width = width;
	img.height = height;
	img.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	img.pixels = (uint8_t*)data.data();
	img.rowPitch = width * 8;
	img.slicePitch = img.rowPitch * height;
	
	HRESULT hr = DirectX::SaveToDDSFile(img, DirectX::DDS_FLAGS_NONE, UTF8ToUTF16(outFile).c_str());
	if (FAILED(hr)) {
		std::cout << "���� DDS ʧ��";
		return 1;
	}
	
	std::cout << "������ " << outFile << std::endl;
	return 0;
}

