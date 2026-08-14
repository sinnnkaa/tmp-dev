"""
Готовит из best.onnx экспорт с тремя "сырыми" головами (по 74 канала), ровно в
том виде, который ожидает blind_nav/src/decode.cpp:
  каналы 0..63  — DFL-регрессия бокса (4 стороны x reg_max=16)
  каналы 64..73 — логиты 10 классов

В самом best.onnx головы уже склеены по-другому (Reshape+Concat по всем
масштабам сразу, выход [1,14,5376] с посчитанным DFL), поэтому граф режется на
шести свёртках головы, а нужные пары складываются обратно тремя Concat.
Так новая модель остаётся drop-in заменой yolo11_final.rknn: те же три выхода
74x64x64 / 74x32x32 / 74x16x16, C++ трогать не нужно.
"""
import sys
import onnx
from onnx import helper, TensorProto, shape_inference

SRC = sys.argv[1]
DST = sys.argv[2]

# (box-ветка, class-ветка, размер сетки) для P3/P4/P5
HEADS = [
    ("/model.23/cv2.0/cv2.0.2/Conv_output_0", "/model.23/cv3.0/cv3.0.2/Conv_output_0", 64),
    ("/model.23/cv2.1/cv2.1.2/Conv_output_0", "/model.23/cv3.1/cv3.1.2/Conv_output_0", 32),
    ("/model.23/cv2.2/cv2.2.2/Conv_output_0", "/model.23/cv3.2/cv3.2.2/Conv_output_0", 16),
]

cut_outputs = [name for pair in HEADS for name in pair[:2]]
tmp = DST + ".cut.onnx"
onnx.utils.extract_model(SRC, tmp, input_names=["images"], output_names=cut_outputs)

m = onnx.load(tmp)
del m.graph.output[:]

for i, (box, cls, grid) in enumerate(HEADS):
    out = f"head_p{i + 3}"
    m.graph.node.append(
        helper.make_node("Concat", inputs=[box, cls], outputs=[out], axis=1, name=f"head_concat_p{i + 3}")
    )
    m.graph.output.append(
        helper.make_tensor_value_info(out, TensorProto.FLOAT, [1, 74, grid, grid])
    )

m = shape_inference.infer_shapes(m)
onnx.checker.check_model(m)
onnx.save(m, DST)

print("выходы нового графа:")
for o in m.graph.output:
    print(" ", o.name, [d.dim_value for d in o.type.tensor_type.shape.dim])
