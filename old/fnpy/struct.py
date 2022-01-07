from dataclasses import dataclass
from .imports import *
import inspect

@dataclass
class RefTy:
    ty: Union['Ty', 'Fn']

def fsizeof(ty: 'Ty'):
    if isinstance(ty, DataTy) or (
            inspect.isclass(ty) and issubclass(ty, DataTy)):
        return sizeof(ty)
    elif isinstance(ty, StructTy):
        return ty.size
    elif isinstance(ty, RefTy):
        return sizeof(Ptr)
    else:
        raise ValueError(f"Has no size: {ty}")

def alignment(size):
    """Return the required alignment for a variable of size."""
    if size > 2: return 2
    return size

def needAlign(ptr, size):
    """Return how much alignment needs to be added to the ptr."""
    if size == 0: return 0
    amnt = alignment(size)
    mod = ptr % amnt
    if mod: return USIZE - mod
    return 0

def alignTy(ptr: int, ty: "Ty"):
    """Calculate correct ptr for dataTy."""
    size = fsizeof(ty)
    return ptr + needAlign(ptr, size)

def calcOffsetsAndSize(fields: List["Ty"]):
    offset = 0
    offsets = []
    for ty in fields:
        offset = alignTy(offset, ty)
        offsets.append(offset)
        offset += fsizeof(ty)
    return offsets, offset

@dataclass
class Field:
    name: str
    ty: "Ty"
    index: int
    offset: int

def assertAllStkTypes(tys):
    for ty in tys:
        assert fsizeof(ty) in {USIZE, 2 * USIZE}, ty

def assertNoStks(tys):
    for ty in tys:
        if isinstance(ty, StructTy) and ty.isStk:
            raise TypeError("Cannot have isStk structs inside structs.")

class StructTy:
    """Create a struct data type from core types."""

    def __init__(self, fields: List[Tuple[str, DataTy]], isStk=False):
        self.isStk = isStk
        self.names = list(map(operator.itemgetter(0), fields))
        self.tys = list(map(operator.itemgetter(1), fields))
        self.offsets, self.size = calcOffsetsAndSize(self.tys)
        self.fields = []
        self.fieldMap = {}
        for i in range(len(self.names)):
            name = self.names[i]
            field = Field(
                name=name,
                ty=self.tys[i],
                index=i,
                offset=self.offsets[i],
            )
            self.fields.append(field)
            if name is None:
                continue
            self.fieldMap[name] = field

        if isStk: assertAllStkTypes(self.tys)
        else: assertNoStks(self.tys)

    def field(self, key: str):
        """Return the field given a '.' separated key.

        For use with nested structs.
        """
        st = self
        field = None
        try:
            for k in key.split('.'):
                field = _getField(st, k)
                st = field.ty
        except KeyError as e:
            raise KeyError(f"{key}: {e}")
        return field

    def offset(self, key: str):
        """Return the offset of a field given a '.' separated key.

        The offset is calculated with respect to self (the "base" struct).
        """
        st = self
        offset = 0
        try:
            for k in key.split('.'):
                field = _getField(st, k)
                offset += field.offset
                st = field.ty
        except KeyError as e:
            raise KeyError(f"{key}: {e}")
        return offset

    def ty(self, key: str):
        return self.field(key).ty

    def __len__(self):
        return len(self.offsets)

    def __getitem__(self, item: str):
        return self.field(item)


def _prepareSubKey(k):
    if isinstance(k, int): return k
    k = k.strip()
    try: k = int(k)
    except ValueError: pass
    return k

def _getField(struct, k: Union[int, str]):
    k = k.strip()
    try:
        k = int(k)
        return struct.fields[k]
    except ValueError: pass
    return struct.fieldMap[k]


def testStruct():
    aFields = [
        ('a1', U32),
        ('a2', U8),
        ('a3', U16),
        ('a4', U16),
        ('a5', U64),
        ('a6', U8),
    ]
    a = StructTy(aFields)
    assert 0 == a['a1'].offset
    assert 4 == a['a2'].offset
    assert 6 == a['a3'].offset
    assert 8 == a['a4'].offset
    assert 12 == a['a5'].offset
    assert 20 == a['a6'].offset
    assert 21 == a.size

    bFields = [
        ('u8', U8),
        ('a', a),
        ('u32', U32),
    ]

    b = StructTy(bFields)
    assert 0 == b['u8'].offset
    assert 4 == b['a'].offset
    assert 4 == b.offset('a.a1')
    assert 8 == b.offset('a.a2')
    assert 28 == b['u32'].offset
    assert 32 == b.size

    class C_A(ctypes.Structure):
        _fields_ = [
            ('a1', U32),
            ('a2', U8),
            ('a3', U16),
            ('a4', U16),
            # Note: a5 must be split into two U32's. Python's ctypes is running
            # in 64bit mode which has a maxalign of 8 bytes
            ('a5', U32),
            ('a5_', U32),
            ('a6', U8),
        ]

    assert C_A.a1.offset == a['a1'].offset
    assert C_A.a2.offset == a['a2'].offset
    assert C_A.a3.offset == a['a3'].offset
    assert C_A.a4.offset == a['a4'].offset
    assert C_A.a5.offset == a['a5'].offset
    assert C_A.a6.offset == a['a6'].offset

    class C_B(ctypes.Structure):
        _fields_ = [
            ('u8', U8),
            ('a', C_A),
            ('u32', U32),
        ]

    assert C_B.u8.offset == b['u8'].offset
    assert C_B.a.offset == b['a'].offset
    assert C_B.u32.offset == b['u32'].offset


Void = StructTy([])


class FnStructTy(StructTy):
    """The structure that is pushed/popped from the return stack
    when a function is called that holds the function's inputs,
    outputs and local values.
    """
    def __init__(
            self,
            inp: StructTy,
            ret: StructTy,
            locals_: StructTy):

        if not (ret is Void or type(ret) is RefTy):
            raise TypeError(f'ret {ret}')

        super().__init__([
            ('ret', ret),
            ('inp', inp),
            ('locals', locals_),
        ])


class Fn(object):
    def __init__(self, name: str, struct: FnStructTy):
        self.name, self.struct = name, struct


Ty = Union[DataTy, StructTy, RefTy]