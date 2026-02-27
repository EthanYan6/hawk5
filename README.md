# hawk5 🦅

`main` 分支是原仓库内容
`motorola_r7` 分支是将主页面修改为摩托罗拉R7样式的页面
> 分支上为了Windows上可以打包，修改了一些内容。为方便下载，还将子模块的内容一起提交了。

## Building

```sh
git submodule update --init --recursive --depth=1
make
```

## Flashing

```sh
k5prog -F -YYY -b ./bin/firmware.bin
```

