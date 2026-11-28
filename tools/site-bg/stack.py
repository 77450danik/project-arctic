"""Site background, path traced in Blender (Cycles): a wave of thick rounded
cards in aurora colours, back-lit, with a violet glow and stars behind.

usage: blender -b -P stack.py -- <dark|light> <out.png> <width> <height> <samples>
The site uses 2560x1100 at 64 samples: site/assets/bg-<theme>.webp (+ a
1280-wide copy), a band at the very bottom of the page.
"""
import math
import sys

import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
THEME, OUT, W, H, SAMPLES = argv[0], argv[1], int(argv[2]), int(argv[3]), int(argv[4])
LIGHT = THEME == "light"

N = 72
SPACING = 0.15
THICK = 0.075
DEPTH = 2.6            # how far each card runs back
BOTTOM = -3.0


def wave(x):
    return 0.6 * (0.75 * math.sin(0.52 * x + 0.5)
                  + 0.38 * math.sin(1.25 * x + 2.2)
                  + 0.16 * math.sin(2.6 * x + 0.7))


def clear():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def card_material():
    m = bpy.data.materials.new("card")
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    info = nt.nodes.new("ShaderNodeObjectInfo")
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    rng = nt.nodes.new("ShaderNodeMapRange")
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    half = N * SPACING / 2
    rng.inputs["From Min"].default_value = -half
    rng.inputs["From Max"].default_value = half
    nt.links.new(info.outputs["Location"], sep.inputs[0])
    nt.links.new(sep.outputs["X"], rng.inputs["Value"])
    nt.links.new(rng.outputs["Result"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    els = ramp.color_ramp.elements
    if LIGHT:
        stops = [(0.0, (0.20, 0.75, 0.45)), (0.35, (0.15, 0.70, 0.80)), (0.65, (0.30, 0.45, 1.00)), (1.0, (0.62, 0.40, 1.00))]
    else:
        stops = [(0.0, (0.10, 0.80, 0.25)), (0.33, (0.03, 0.62, 0.62)), (0.62, (0.16, 0.30, 1.00)), (1.0, (0.58, 0.20, 1.00))]
    els[0].position, els[0].color = stops[0][0], (*stops[0][1], 1)
    els[1].position, els[1].color = stops[-1][0], (*stops[-1][1], 1)
    for pos, c in stops[1:-1]:
        e = els.new(pos)
        e.color = (*c, 1)
    bsdf.inputs["Roughness"].default_value = 0.32
    bsdf.inputs["Coat Weight"].default_value = 0.35
    bsdf.inputs["Coat Roughness"].default_value = 0.12
    return m


def build_cards(mat):
    for i in range(N):
        x = (i - (N - 1) / 2) * SPACING
        top = wave(x)
        verts = []
        steps = 48
        # the top edge is an arch over the card's depth
        for k in range(steps + 1):
            y = -DEPTH / 2 + DEPTH * k / steps
            arch = math.cos(math.pi * (y / DEPTH)) ** 0.6
            z = BOTTOM + (top - BOTTOM) * 1.0 + 0.0
            z = top - 0.55 * (1 - arch)
            verts.append((0.0, y, z))
        verts.append((0.0, DEPTH / 2, BOTTOM))
        verts.append((0.0, -DEPTH / 2, BOTTOM))
        me = bpy.data.meshes.new(f"card{i}")
        me.from_pydata(verts, [], [list(range(len(verts)))])
        ob = bpy.data.objects.new(f"card{i}", me)
        ob.location.x = x
        bpy.context.collection.objects.link(ob)
        ob.data.materials.append(mat)
        sol = ob.modifiers.new("solid", "SOLIDIFY")
        sol.thickness = THICK
        sol.offset = 0
        bev = ob.modifiers.new("bevel", "BEVEL")
        bev.width = THICK * 0.48
        bev.segments = 6
        bev.limit_method = "ANGLE"
        bev.harden_normals = True
        for p in me.polygons:
            p.use_smooth = True


def backdrop():
    """A big emissive wall far behind: dark gradient, violet glow, stars."""
    bpy.ops.mesh.primitive_plane_add(size=1, location=(0, 14, 1.0), rotation=(math.pi / 2, 0, 0))
    wall = bpy.context.object
    wall.scale = (60, 34, 1)
    wall.location = (1.5, 14, -1.0)
    m = bpy.data.materials.new("wall")
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    em = nt.nodes.new("ShaderNodeEmission")
    tc = nt.nodes.new("ShaderNodeTexCoord")
    mp = nt.nodes.new("ShaderNodeMapping")
    grad = nt.nodes.new("ShaderNodeTexGradient")
    grad.gradient_type = "SPHERICAL"
    mp.inputs["Location"].default_value = (0.0, 0.0, 0.0)
    mp.inputs["Scale"].default_value = (2.9, 4.4, 1.0)
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    els = ramp.color_ramp.elements
    if LIGHT:
        els[0].color = (0.80, 0.85, 0.97, 1)
        els[1].position, els[1].color = 1.0, (0.90, 0.84, 1.0, 1)
    else:
        els[0].color = (0.004, 0.002, 0.012, 1)
        els[-1].position, els[-1].color = 1.0, (0.36, 0.08, 0.80, 1)
    vor = nt.nodes.new("ShaderNodeTexVoronoi")
    vor.inputs["Scale"].default_value = 260.0
    vor.inputs["Randomness"].default_value = 1.0
    st = nt.nodes.new("ShaderNodeMapRange")       # distance -> star dot
    st.inputs["From Min"].default_value = 0.035
    st.inputs["From Max"].default_value = 0.0
    st.clamp = True
    sel = nt.nodes.new("ShaderNodeMath")          # only some cells hold a star
    sel.operation = "GREATER_THAN"
    sel.inputs[1].default_value = 0.93
    sep = nt.nodes.new("ShaderNodeSeparateColor")
    mul = nt.nodes.new("ShaderNodeMath")
    mul.operation = "MULTIPLY"
    mul2 = nt.nodes.new("ShaderNodeMath")
    mul2.operation = "MULTIPLY"
    mul2.inputs[1].default_value = 0.0 if LIGHT else 6.0
    add = nt.nodes.new("ShaderNodeMix")
    add.data_type = "RGBA"
    add.blend_type = "ADD"
    add.inputs["Factor"].default_value = 1.0
    nt.links.new(tc.outputs["Object"], mp.inputs["Vector"])
    nt.links.new(mp.outputs["Vector"], grad.inputs["Vector"])
    soft = nt.nodes.new("ShaderNodeMath")         # smooth fall-off, no visible rim
    soft.operation = "POWER"
    soft.inputs[1].default_value = 2.6
    nt.links.new(grad.outputs["Fac"], soft.inputs[0])
    nt.links.new(soft.outputs["Value"], ramp.inputs["Fac"])
    nt.links.new(tc.outputs["Object"], vor.inputs["Vector"])
    nt.links.new(vor.outputs["Distance"], st.inputs["Value"])
    nt.links.new(vor.outputs["Color"], sep.inputs["Color"])
    nt.links.new(sep.outputs["Red"], sel.inputs[0])
    nt.links.new(st.outputs["Result"], mul.inputs[0])
    nt.links.new(sel.outputs["Value"], mul.inputs[1])
    nt.links.new(mul.outputs["Value"], mul2.inputs[0])
    nt.links.new(ramp.outputs["Color"], add.inputs["A"])
    nt.links.new(mul2.outputs["Value"], add.inputs["B"])
    nt.links.new(add.outputs["Result"], em.inputs["Color"])
    em.inputs["Strength"].default_value = 1.9 if LIGHT else 1.0
    nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
    wall.data.materials.append(m)
    wall.visible_shadow = False
    wall.visible_diffuse = False


def area(name, loc, rot, size, size_y, energy, color):
    ld = bpy.data.lights.new(name, "AREA")
    ld.shape = "RECTANGLE"
    ld.size, ld.size_y = size, size_y
    ld.energy = energy
    ld.color = color
    ob = bpy.data.objects.new(name, ld)
    ob.location = loc
    ob.rotation_euler = rot
    bpy.context.collection.objects.link(ob)


def lights():
    if LIGHT:
        area("back", (0, 5.0, 4.5), (math.radians(-125), 0, 0), 16, 3, 2200, (1.0, 0.97, 1.0))
        area("fill", (-4, -6, 3.5), (math.radians(60), 0, math.radians(-30)), 10, 6, 1400, (0.9, 0.95, 1.0))
        area("top", (0, 0, 7), (0, 0, 0), 14, 8, 900, (1, 1, 1))
    else:
        area("back", (0, 5.0, 4.0), (math.radians(-125), 0, 0), 18, 2.5, 3000, (0.90, 0.85, 1.0))
        area("fill", (-5, -7, 2.0), (math.radians(70), 0, math.radians(-35)), 10, 6, 700, (0.75, 0.88, 1.0))
        area("rim", (6, 2, 2.5), (math.radians(-100), 0, math.radians(110)), 6, 2, 260, (0.70, 0.40, 1.0))


def camera():
    cd = bpy.data.cameras.new("cam")
    cd.lens = 36
    cd.dof.use_dof = True
    cd.dof.aperture_fstop = 4.0
    ob = bpy.data.objects.new("cam", cd)
    ob.location = (1.2, -9.6, 1.5)
    bpy.context.collection.objects.link(ob)
    target = bpy.data.objects.new("target", None)
    target.location = (-0.3, 0.0, 1.45)
    bpy.context.collection.objects.link(target)
    c = ob.constraints.new("TRACK_TO")
    c.target = target
    c.track_axis = "TRACK_NEGATIVE_Z"
    c.up_axis = "UP_Y"
    cd.dof.focus_object = None
    cd.dof.focus_distance = 9.5
    bpy.context.scene.camera = ob


def render_settings():
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    prefs = bpy.context.preferences.addons["cycles"].preferences
    prefs.compute_device_type = "CUDA"
    prefs.get_devices()
    for d in prefs.devices:
        d.use = d.type == "CUDA"
    sc.cycles.device = "GPU"
    sc.cycles.samples = SAMPLES
    sc.cycles.use_denoising = True
    sc.cycles.denoiser = "OPENIMAGEDENOISE"
    sc.cycles.max_bounces = 6
    sc.render.resolution_x, sc.render.resolution_y = W, H
    sc.render.resolution_percentage = 100
    sc.render.dither_intensity = 1.0
    sc.view_settings.view_transform = "AgX"
    sc.view_settings.look = "AgX - Punchy" if not LIGHT else "AgX - Medium High Contrast"
    sc.view_settings.exposure = 0.0
    sc.render.image_settings.file_format = "PNG"
    sc.render.image_settings.color_depth = "8"
    sc.render.filepath = OUT
    world = bpy.data.worlds.new("w")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (0.85, 0.88, 0.97, 1) if LIGHT else (0.004, 0.002, 0.010, 1)
    bg.inputs["Strength"].default_value = 0.5 if LIGHT else 1.0
    sc.world = world


clear()
build_cards(card_material())
backdrop()
lights()
camera()
render_settings()
bpy.ops.render.render(write_still=True)
