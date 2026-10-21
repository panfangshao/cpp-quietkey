// 生成 quietkey 的多尺寸 .ico —— 深蓝圆底 + 两根白色竖条（暂停符号）。
//
// 构建期跑一次，产物由 app.rc 编进 exe 的资源段。图案只有这一处定义，
// 仓库里不放二进制资源文件，改样式只改这个文件。
//
// 刻意不依赖任何库：全部条目都写成 32 位 BMP（DIB）。
// 256×256 用 PNG 更省体积，但那要引入 zlib，不值得——
// Vista 之后 256 的 BMP 条目一样能正常显示。
//
// 另一个坑：不要用"自动挑选编码方式"的做法把小尺寸也压成 PNG。
// 小尺寸的 PNG 条目在部分 shell 路径下读不出来，图标会变空白。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

struct Rgba {
    std::vector<uint8_t> px;  // RGBA，行优先，从上到下
    uint32_t size;
};

// 像素 [p, p+1) 与区间 [a, b) 的重叠长度，用作抗锯齿覆盖率。
float Overlap(float p, float a, float b) {
    float v = (std::min)(p + 1.0f, b) - (std::max)(p, a);
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

uint8_t Mix(uint8_t from, uint8_t to, float t) {
    return static_cast<uint8_t>(from + (to - from) * t + 0.5f);
}

// 按 32px 的原始设计等比缩放，圆边和竖条边缘都做覆盖率抗锯齿，
// 16px 的托盘尺寸和 256px 的大图标都不会糊。
Rgba Render(uint32_t size) {
    const uint8_t bg[3] = {32, 96, 190};
    const uint8_t fg[3] = {255, 255, 255};

    Rgba out;
    out.size = size;
    out.px.assign(static_cast<size_t>(size) * size * 4, 0);

    const float s = static_cast<float>(size);
    const float center = s / 2.0f;
    const float radius = center - (std::max)(s / 32.0f, 0.5f);  // 留半像素余量

    const float k = s / 32.0f;
    const float bars[2][2] = {{9.0f * k, 13.0f * k}, {19.0f * k, 23.0f * k}};
    const float barTop = 9.0f * k;
    const float barBottom = 23.0f * k;

    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float dx = x + 0.5f - center;
            const float dy = y + 0.5f - center;
            float disc = radius - std::sqrt(dx * dx + dy * dy) + 0.5f;
            disc = disc < 0.0f ? 0.0f : (disc > 1.0f ? 1.0f : disc);
            if (disc <= 0.0f) {
                continue;
            }

            const float fx = static_cast<float>(x);
            const float barX = (std::max)(Overlap(fx, bars[0][0], bars[0][1]),
                                          Overlap(fx, bars[1][0], bars[1][1]));
            const float bar = barX * Overlap(static_cast<float>(y), barTop, barBottom);

            const size_t i = (static_cast<size_t>(y) * size + x) * 4;
            out.px[i + 0] = Mix(bg[0], fg[0], bar);
            out.px[i + 1] = Mix(bg[1], fg[1], bar);
            out.px[i + 2] = Mix(bg[2], fg[2], bar);
            out.px[i + 3] = static_cast<uint8_t>(disc * 255.0f + 0.5f);
        }
    }
    return out;
}

void PutU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void PutU32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) {
        v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
    }
}

// 一个 ICO 条目的图像数据：BITMAPINFOHEADER + BGRA 位图（自下而上）+ AND 掩码。
// AND 掩码即便全 0 也必须写，缺了它有些 shell 路径会画不出来。
std::vector<uint8_t> EncodeBmpEntry(const Rgba& img) {
    const uint32_t n = img.size;
    std::vector<uint8_t> out;

    PutU32(out, 40);          // biSize
    PutU32(out, n);           // biWidth
    PutU32(out, n * 2);       // biHeight：图像 + 掩码，所以是两倍
    PutU16(out, 1);           // biPlanes
    PutU16(out, 32);          // biBitCount
    PutU32(out, 0);           // biCompression = BI_RGB
    PutU32(out, 0);           // biSizeImage
    PutU32(out, 0);           // biXPelsPerMeter
    PutU32(out, 0);           // biYPelsPerMeter
    PutU32(out, 0);           // biClrUsed
    PutU32(out, 0);           // biClrImportant

    for (uint32_t row = 0; row < n; ++row) {
        const uint32_t y = n - 1 - row;  // 自下而上
        for (uint32_t x = 0; x < n; ++x) {
            const size_t i = (static_cast<size_t>(y) * n + x) * 4;
            out.push_back(img.px[i + 2]);  // B
            out.push_back(img.px[i + 1]);  // G
            out.push_back(img.px[i + 0]);  // R
            out.push_back(img.px[i + 3]);  // A
        }
    }

    // AND 掩码：1bpp，每行按 4 字节对齐。32 位图靠 alpha 通道透明，掩码全 0 即可。
    const uint32_t maskRowBytes = ((n + 31) / 32) * 4;
    out.insert(out.end(), static_cast<size_t>(maskRowBytes) * n, 0);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "用法: make_icon <输出路径.ico>\n");
        return 2;
    }

    // 小到托盘 16px、大到资源管理器超大图标 256px 各备一份，
    // 否则 Windows 会拿最近的尺寸硬缩，边缘发糊。
    const uint32_t sizes[] = {16, 20, 24, 32, 48, 64, 128, 256};
    const uint16_t count = static_cast<uint16_t>(sizeof(sizes) / sizeof(sizes[0]));

    std::vector<std::vector<uint8_t>> images;
    images.reserve(count);
    for (uint32_t s : sizes) {
        images.push_back(EncodeBmpEntry(Render(s)));
    }

    std::vector<uint8_t> file;
    PutU16(file, 0);      // 保留
    PutU16(file, 1);      // 类型：1 = 图标
    PutU16(file, count);

    uint32_t offset = 6u + 16u * count;
    for (uint16_t i = 0; i < count; ++i) {
        const uint32_t s = sizes[i];
        file.push_back(static_cast<uint8_t>(s == 256 ? 0 : s));  // 256 记作 0
        file.push_back(static_cast<uint8_t>(s == 256 ? 0 : s));
        file.push_back(0);  // 调色板色数
        file.push_back(0);  // 保留
        PutU16(file, 1);    // 平面数
        PutU16(file, 32);   // 位深
        PutU32(file, static_cast<uint32_t>(images[i].size()));
        PutU32(file, offset);
        offset += static_cast<uint32_t>(images[i].size());
    }
    for (const auto& img : images) {
        file.insert(file.end(), img.begin(), img.end());
    }

    FILE* fp = std::fopen(argv[1], "wb");
    if (!fp) {
        std::fprintf(stderr, "无法写入 %s\n", argv[1]);
        return 1;
    }
    const size_t wrote = std::fwrite(file.data(), 1, file.size(), fp);
    std::fclose(fp);
    if (wrote != file.size()) {
        std::fprintf(stderr, "写入不完整\n");
        return 1;
    }
    std::printf("生成 %s（%u 档尺寸，%zu 字节）\n", argv[1], count, file.size());
    return 0;
}
