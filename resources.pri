# 主程序和测试共用资源及压缩设置；解包后的文件内容保持不变。
RESOURCES += $$PWD/resources.qrc

# PE 工具的压缩收益未达到 rcc 默认阈值，显式保留所有有收益的 zlib 压缩。
QMAKE_RESOURCE_FLAGS += --compress-algo zlib --compress 9 --threshold 0
