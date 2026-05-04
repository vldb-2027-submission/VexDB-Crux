import struct
import sys
import os

def fbin_to_sql(fbin_path, sql_path):
    """
    将 fbin 二进制文件转换为 sql 文件
    """
    if not os.path.exists(fbin_path):
        print(f"错误: 找不到文件 {fbin_path}")
        return

    try:
        with open(fbin_path, 'rb') as f_in, open(sql_path, 'w', encoding='utf-8') as f_out:
            # 1. 读取头信息 (8字节)
            # <II 表示小端字节序 (Little Endian)，两个 unsigned int (32bit)
            header_data = f_in.read(8)
            if len(header_data) < 8:
                print("错误: 文件太小，无法读取头信息")
                return

            num_vectors, vector_dim = struct.unpack('<II', header_data)
            print(f"检测到文件信息 -> 向量数量: {num_vectors}, 维度: {vector_dim}")

            # 计算每个向量占用的字节数 (float32 占 4 字节)
            bytes_per_vector = vector_dim * 4

            # 2. 逐个读取向量并写入 SQL
            count = 0
            for i in range(num_vectors):
                vector_bytes = f_in.read(bytes_per_vector)
                
                # 检查数据完整性
                if len(vector_bytes) < bytes_per_vector:
                    print(f"警告: 在第 {i} 个向量处文件意外结束")
                    break

                # 解析 float32 数组
                # f'<{vector_dim}f' 表示解析 vector_dim 个小端 float
                vector_tuple = struct.unpack(f'<{vector_dim}f', vector_bytes)

                # 3. 格式化为字符串
                # 将 float 转换为 string，并用逗号连接
                # 控制小数点后6位精度
                vector_str_content = ','.join(f"{v:.6f}" for v in vector_tuple)
                
                # 拼接 SQL 语句
                # 格式: execute p1('[xxx,xxx,xxx]');
                sql_line = f"execute p1('[{vector_str_content}]');\n"
                
                f_out.write(sql_line)
                count += 1

                # 可选：打印进度
                if count % 1000 == 0:
                    print(f"已处理 {count}/{num_vectors} 行...", end='\r')

            print(f"\n转换完成！成功生成: {sql_path}")
            print(f"共处理向量: {count} 条")

    except Exception as e:
        print(f"发生错误: {e}")

if __name__ == "__main__":
    # 使用示例
    # 你可以直接修改下面的路径，或者通过命令行参数传入
    
    # 示例：假设输入文件名为 data.fbin，输出为 output.sql
    input_file = 'data.fbin'
    output_file = 'output.sql'

    # 如果通过命令行传参: python script.py input.fbin output.sql
    if len(sys.argv) >= 3:
        input_file = sys.argv[1]
        output_file = sys.argv[2]

    print(f"正在将 {input_file} 转换为 {output_file} ...")
    fbin_to_sql(input_file, output_file)