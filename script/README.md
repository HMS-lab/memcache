# script

## 1 install dir

```text
${INSTALL_PATH}/
          |--memcache_hybrid
              |-- latest
              |-- set_env.sh
              |-- ${version}
                   |-- ${arch}-${os}
                        |-- include    (header files)
                        |-- bin        (TLS-related binaries)
                        |-- lib64      (shared libraries)
                        |-- whl        (Python wheel package)
                   |-- uninstall.sh
                   |-- version.info


default ${INSTALL_PATH} is /usr/local/
```

## 2 rule of package name

```shell
memcache_hybrid-${version}_${os}_${arch}.run
```

## 3 upgrade

support offline upgrade

## 4 where is the package

built from gitee and placed on gitee for downloading

## 5 check library version

user can get library version by linux 'strings' command

example to get the library version using 'strings' as following:

```shell
strings libmf_smem.so | grep commit

library version: 1.0.0, build time: Apr 27 2025 08:46:17, commit: 4ad27e5b4bd3353c5c20f16e8f3b6da41268d4e0
```
