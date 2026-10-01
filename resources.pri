# 主程序和测试共用资源及压缩设置；解包后的文件内容保持不变。
RESOURCES += $$PWD/resources.qrc

# 默认针对下载包：不压缩内嵌资源，交给外层 7z 统一压缩。
# uncompressed_resources 保留为兼容旧命令的显式别名。
contains(CONFIG, compressed_resources) {
    contains(CONFIG, uncompressed_resources) {
        error("compressed_resources and uncompressed_resources cannot be used together")
    }
    # 可选恢复旧的 zlib 资源压缩；适合优先缩小解压后 exe 的构建。
    QMAKE_RESOURCE_FLAGS += --compress-algo zlib --compress 9 --threshold 0
} else {
    QMAKE_RESOURCE_FLAGS += --no-compress
}

# 资源配置修改后重新生成 RCC 源码，避免沿用旧构建目录的资源数据。
rcc.depends += $$PWD/resources.pri
