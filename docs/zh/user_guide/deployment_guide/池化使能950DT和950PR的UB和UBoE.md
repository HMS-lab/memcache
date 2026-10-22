# 池化使能950DT和950PR的UB和UBoE

## 1. 前置条件

宿主机执行如下命令，关闭验签：

```bash
此命令执行完，需要手动全部确认：Y
for i in {0..7}; do npu-smi set -t custom-op-secverify-enable -i $i -d 1; done;

for i in {0..7}; do npu-smi set -t custom-op-secverify-mode -i $i -d 0; done;
```

## 2. 协议选择

### 在 950 Pod机型中

```toml
ock.mmc.local_service.protocol = device_urma
```

使用的是6个出框口

### 在 950 Server机型中

```toml
ock.mmc.local_service.protocol = device_urma / device_uboe
```

device_urma：使用的是die0的8个出框口

device_uboe：使用是die1的1个参数面口

## FAQ

1. 推理报错 device_urma launch kernel AclrtSynchronizeStream failed, ret: 507018

![image.png](https://raw.gitcode.com/user-images/assets/7672915/5e10fbbf-71a7-4131-a113-296895fba2ab/image.png 'image.png')

查看对应这个1003进程的plog日志，如果出现下面的错误：

```ini
PrintAicpuErrorInfo:AI CPU kernel execution failed, device_id=1, stream_id=280, task_pos=0, fault op_name=, soName=libcann_hybm_kernel.so, funcName=HybmBatchWrite, kernelName=HybmBatchWrite
```

则需要在启动推理进程进程之前执行，手动导入一次memfabric_hybrid的包：

```python
python -c "import memfabric_hybrid"
```
