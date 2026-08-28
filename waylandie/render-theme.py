import json, re, os

TPL = "/root/.config/quickshell/ii/scripts/colors/terminal/kitty-theme.conf"
TPL_SEQ = "/root/.config/quickshell/ii/scripts/colors/terminal/sequences.txt"
OUTDIR = "/root/.local/state/quickshell/user/generated/terminal/"

with open("/root/.local/state/quickshell/user/generated/colors.json") as f:
    c = json.load(f)

# colors.json values already include '#' — strip it so the template's
# literal '#' prefix produces a single '#rrggbb'.
def plain(v):
    return v.lstrip("#") if isinstance(v, str) else v

term = [
    c["surface_container_lowest"], c["error"], c["primary"], c["tertiary"],
    c["primary_fixed"], c["secondary"], c["on_primary_container"], c["on_surface"],
    c["outline"], c["error_container"], c["primary_fixed_dim"], c["on_tertiary_container"],
    c["primary_container"], c["secondary_fixed_dim"], c["tertiary_container"], c["inverse_surface"],
]

env = {f"term{i}": plain(term[i]) for i in range(16)}
env.update({
    "termbg": plain(c["background"]), "termfg": plain(c["on_background"]), "termcursor": plain(c["primary"]),
    "background": plain(c["background"]), "foreground": plain(c["on_background"]), "cursor": plain(c["primary"]),
    "selection_background": plain(c["surface_container_high"]), "selection_foreground": plain(c["on_surface"]),
    "url_color": plain(c["tertiary"]),
})
for k, v in c.items():
    env[k] = plain(v)
    parts = k.split("_")
    env[parts[0] + "".join(p.capitalize() for p in parts[1:])] = plain(v)

os.makedirs(OUTDIR, exist_ok=True)
for tpl in (TPL, TPL_SEQ):
    if not os.path.exists(tpl):
        continue
    with open(tpl) as f:
        content = f.read()
    for k, v in sorted(env.items(), key=lambda x: -len(x[0])):
        content = content.replace("$" + k, v)
    out = OUTDIR + os.path.basename(tpl)
    with open(out, "w") as f:
        f.write(content)
    leftover = re.findall(r"\$[a-zA-Z_]+", content)
    dbl = content.count("##")
    print(f"{os.path.basename(tpl)}: leftover={leftover[:3] if leftover else 'NONE'} double-hash={dbl}")
