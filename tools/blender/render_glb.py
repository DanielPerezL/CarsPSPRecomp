"""Renderiza un .glb a PNG para revisión rápida.
Uso: blender -b --factory-startup --python tools/blender/render_glb.py -- modelo.glb salida.png [azimut] [elevación] [zoom]
"""
import math
import sys

import bpy
from mathutils import Vector

args = sys.argv[sys.argv.index("--") + 1:]
src, dst = args[0], args[1]
azimuth = math.radians(float(args[2])) if len(args) > 2 else math.radians(35)
elevation = math.radians(float(args[3])) if len(args) > 3 else math.radians(25)
zoom = float(args[4]) if len(args) > 4 else 1.0

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
pts = [o.matrix_world @ Vector(c) for o in meshes for c in o.bound_box]
lo = Vector([min(p[i] for p in pts) for i in range(3)])
hi = Vector([max(p[i] for p in pts) for i in range(3)])
center, radius = (lo + hi) / 2, max((hi - lo).length / 2, 0.01)

cam_data = bpy.data.cameras.new("cam")
cam_data.lens = 50
cam = bpy.data.objects.new("cam", cam_data)
bpy.context.scene.collection.objects.link(cam)
dist = radius / math.tan(cam_data.angle / 2) * 1.1 / zoom
# glTF Y-arriba se importa como Z-arriba en Blender
horiz = math.cos(elevation) * dist
cam.location = center + Vector((math.sin(azimuth) * horiz, -math.cos(azimuth) * horiz, math.sin(elevation) * dist))
cam.rotation_euler = (center - cam.location).to_track_quat("-Z", "Y").to_euler()
cam_data.clip_end = dist * 10
scene = bpy.context.scene
scene.camera = cam
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.light = "STUDIO"
scene.display.shading.color_type = "TEXTURE"
scene.render.resolution_x, scene.render.resolution_y = 800, 600
scene.render.filepath = dst
bpy.ops.render.render(write_still=True)
