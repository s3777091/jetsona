# Bản vá cho thư viện bên thứ ba

`third_party/lvgl` là submodule trỏ thẳng tới upstream `github.com/lvgl/lvgl`,
nên các sửa đổi cục bộ không thể commit vào đó. Chúng được giữ ở đây dưới dạng
patch để không biến mất khi clone lại hoặc khi submodule được cập nhật.

## `lvgl-tiny-ttf-missing-glyph.patch`

Sửa `src/libs/tiny_ttf/lv_tiny_ttf.c`. Khi font TTF không có glyph cho một mã
ký tự, `lv_cache_acquire_or_create()` thất bại và LVGL ghi log
`"cache not allocated"` — thông báo sai bản chất, đồng thời chặn `lv_font.c`
thử font dự phòng. Bản vá tra cache trước, phân biệt trường hợp thiếu glyph
bình thường với lỗi cấp phát thật, và sửa lại nội dung hai dòng log.

Áp dụng sau khi clone hoặc sau khi cập nhật submodule:

```bash
cd third_party/lvgl
git apply ../patches/lvgl-tiny-ttf-missing-glyph.patch
```

Kiểm tra trước khi áp (không thay đổi gì): thêm `--check` vào lệnh trên.
Nếu bản vá đã được áp sẵn, `git apply` sẽ báo lỗi — dùng
`git apply --reverse --check` để xác nhận.
