"""pushad/popad save and restore all eight registers (they were RECOMP_UNIMPL)."""
from . import config
from .translator import FunctionTranslator


def _translate(image):
    base = 0x10000
    config._install([config.Section('.text', base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base, origin='pushad')
    db = {base: {'start': hex(base), 'end': base + len(image), '_addr': base, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(base, db[base])


def test_pushad_popad_lifted():
    # pushad; mov eax, 0x11111111; mov ebx, 0x22222222; popad; ret
    code = _translate(bytes.fromhex('60' 'b811111111' 'bb22222222' '61' 'c3'))
    assert 'RECOMP_UNIMPL' not in code
    push = code.index('/* pushal */')
    pop = code.index('/* popal */')
    assert push < pop
    order = ['PUSH32(esp, eax)', 'PUSH32(esp, ecx)', 'PUSH32(esp, edx)', 'PUSH32(esp, ebx)',
             'PUSH32(esp, _pa_esp)', 'PUSH32(esp, ebp)', 'PUSH32(esp, esi)', 'PUSH32(esp, edi)']
    pos = [code.index(p) for p in order]
    assert pos == sorted(pos)
    rest = code[code.rindex('POP32(esp, edi)', 0, pop + 20):pop]
    assert rest.index('POP32(esp, ebp)') < rest.index('esp += 4') < rest.index('POP32(esp, ebx)')
    assert rest.rindex('POP32(esp, eax)') > rest.index('POP32(esp, ecx)')


def test_pushad_declares_ebp_in_frameless_function():
    # pushad; popad; ret -- no operand names ebp, but both instructions use it
    code = _translate(bytes.fromhex('60' '61' 'c3'))
    assert 'uint32_t ebp' in code
