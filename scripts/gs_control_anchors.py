"""Exact-anchor helpers shared by the GS control source transformations."""
def replace_once(text, old, new, label):
    if text.count(old) != 1:
        raise RuntimeError(f"{label}: expected one source anchor, got {text.count(old)}")
    return text.replace(old, new, 1)

def replace_once_in_region(text, start, end, old, new, label):
    a = text.index(start)
    b = text.index(end, a + len(start))
    return text[:a] + replace_once(text[a:b], old, new, label) + text[b:]
