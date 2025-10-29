-- https://gist.github.com/Sainan/02c3ac9cea5015341412c92feec95e56

local function get_bias(byte_length)
    local bias = 0
    while byte_length > 1 do
        byte_length = byte_length - 1
        bias = (bias + 1) << 7
    end
    return bias
end

function pack_u64_dyn(v)
    local out = {}
    for _ = 1, 8 do
        local cur = v & 0x7f
        v = v >> 7
        if v ~= 0 then
            out[#out + 1] = string.char(cur | 0x80)
        else
            out[#out + 1] = string.char(cur)
            return table.concat(out)
        end
    end
    out[#out + 1] = string.char(v)
    return table.concat(out)
end

function unpack_u64_dyn(str, i)
    i = i or 1
    local v = 0
    local bits = 0
    for _ = 1, 8 do
        local b = str:byte(i) i = i + 1
        v = v + ((b & 0x7f) << bits)
        if (b >> 7) == 0 then
            return v, i
        end
        bits = bits + 7
    end
    local b = str:byte(i) i = i + 1
    v = v + (b << 56)
    return v, i
end

function pack_u64_dyn_p(u)
    local byte_length = 1
    if not math.ult(u, 1 << 7) then byte_length = byte_length + 1 end
    if not math.ult(u, 1 << 14) then byte_length = byte_length + 1 end
    if not math.ult(u, 1 << 21) then byte_length = byte_length + 1 end
    if not math.ult(u, 1 << 28) then byte_length = byte_length + 1 end
    if not math.ult(u, 1 << 35) then byte_length = byte_length + 1 end
    if not math.ult(u, 1 << 42) then byte_length = byte_length + 1 end
    if not math.ult(u, 1 << 49) then byte_length = byte_length + 1 end
    if not math.ult(u, 1 << 56) then byte_length = byte_length + 1 end

    local first_byte_value_bits = byte_length < 8 and (8 - byte_length) or 0
    local first_byte_prefix_bits = byte_length - 1

    local first_byte = ((0xff << (8 - first_byte_prefix_bits)) & 0xff) | (u & ((1 << first_byte_value_bits) - 1))
    u = u >> first_byte_value_bits

    local out = { string.char(first_byte) }
    for idx = 1, byte_length - 1 do
        out[#out + 1] = string.char((u >> ((idx - 1) * 8)) & 0xff)
    end
    return table.concat(out)
end

function unpack_u64_dyn_p(str, i)
    i = i or 1
    local first_byte = str:byte(i)

    local prefix_bits = 0
    while prefix_bits < 8 and (first_byte & (0x80 >> prefix_bits)) ~= 0 do
        prefix_bits = prefix_bits + 1
    end

    local byte_length = prefix_bits + 1
    local first_byte_value_bits = byte_length < 8 and (8 - byte_length) or 0

    local v = 0
    for idx = 1, byte_length - 1 do
        local b = str:byte(i + idx)
        v = v | (b << ((idx - 1) * 8))
    end
    v = v << first_byte_value_bits
    local value_mask = first_byte_value_bits == 0 and 0 or ((1 << first_byte_value_bits) - 1)
    v = v | (first_byte & value_mask)

    return v, i + byte_length
end

function pack_u64_dyn_bp(u)
    local byte_length = 1
    if not math.ult(u, 128) then byte_length = byte_length + 1 end
    if not math.ult(u, 16512) then byte_length = byte_length + 1 end
    if not math.ult(u, 2113664) then byte_length = byte_length + 1 end
    if not math.ult(u, 270549120) then byte_length = byte_length + 1 end
    if not math.ult(u, 34630287488) then byte_length = byte_length + 1 end
    if not math.ult(u, 4432676798592) then byte_length = byte_length + 1 end
    if not math.ult(u, 567382630219904) then byte_length = byte_length + 1 end
    if not math.ult(u, 72624976668147840) then byte_length = byte_length + 1 end

    local first_byte_value_bits = byte_length < 8 and (8 - byte_length) or 0
    local first_byte_prefix_bits = byte_length - 1

    u = u - get_bias(byte_length)

    local first_byte = ((0xff << (8 - first_byte_prefix_bits)) & 0xff) | (u & ((1 << first_byte_value_bits) - 1))
    u = u >> first_byte_value_bits

    local out = { string.char(first_byte) }
    for idx = 1, byte_length - 1 do
        out[#out + 1] = string.char((u >> ((idx - 1) * 8)) & 0xff)
    end
    return table.concat(out)
end

function unpack_u64_dyn_bp(str, i)
    i = i or 1
    local first_byte = str:byte(i)

    local prefix_bits = 0
    while prefix_bits < 8 and (first_byte & (0x80 >> prefix_bits)) ~= 0 do
        prefix_bits = prefix_bits + 1
    end

    local byte_length = prefix_bits + 1
    local first_byte_value_bits = byte_length < 8 and (8 - byte_length) or 0

    local v = 0
    for idx = 1, byte_length - 1 do
        local b = str:byte(i + idx)
        v = v | (b << ((idx - 1) * 8))
    end
    v = v << first_byte_value_bits
    local value_mask = first_byte_value_bits == 0 and 0 or ((1 << first_byte_value_bits) - 1)
    v = v | (first_byte & value_mask)

    local bias = get_bias(byte_length)
    assert(not math.ult(0xffffffffffffffff - bias, v))

    return v + bias, i + byte_length
end

function pack_i64_dyn_a(v)
    local neg = (v >> 63)
    if v < 0 then
        v = (~v + 1) & ~(1 << 63)
    end
    return pack_u64_dyn((neg << 6) | ((v & ~0x3f) << 1) | (v & 0x3f))
end

function unpack_i64_dyn_a(str, i)
    local v
    v, i = unpack_u64_dyn(str, i)
    local neg = ((v >> 6) & 1) ~= 0
    v = ((v >> 1) & ~0x3f) | (v & 0x3f)
    if neg then
        v = ~(v - 1) | (1 << 63)
    end
    return v, i
end

function pack_u64_dyn_b(v)
    local out = {}
    for _ = 1, 8 do
        local cur = v & 0x7f
        v = v >> 7
        if v ~= 0 then
            out[#out + 1] = string.char(cur | 0x80)
            v = v - 1 -- bias
        else
            out[#out + 1] = string.char(cur)
            return table.concat(out)
        end
    end
    out[#out + 1] = string.char(v)
    return table.concat(out)
end

function unpack_u64_dyn_b(str, i)
    i = i or 1
    local v = 0
    local bits = 0
    local bias = 0
    for _ = 1, 8 do
        local b = str:byte(i) i = i + 1
        v = v + ((b & 0x7f) << bits)
        if (b >> 7) == 0 then
            goto apply_bias
        end
        bits = bits + 7
        bias = bias + (1 << bits)
    end
    do
        local b = str:byte(i) i = i + 1
        v = v + (b << 56)
    end
::apply_bias::
    assert(not math.ult(0xffffffffffffffff - bias, v))
    return v + bias, i
end

function pack_i64_dyn_b(v)
    local neg = (v >> 63)
    if v < 0 then
        v = ~v
    end
    return pack_u64_dyn_b((neg << 6) | ((v & ~0x3f) << 1) | (v & 0x3f))
end

function unpack_i64_dyn_b(str, i)
    local v
    v, i = unpack_u64_dyn_b(str, i)
    local neg = ((v >> 6) & 1) ~= 0
    v = ((v >> 1) & ~0x3f) | (v & 0x3f)
    if neg then
        v = ~v
    end
    return v, i
end

function pack_i64_dyn_bp(v)
    local neg = (v >> 63)
    if v < 0 then
        v = ~v
    end
    return pack_u64_dyn_bp((neg << 6) | ((v & ~0x3f) << 1) | (v & 0x3f))
end

function unpack_i64_dyn_bp(str, i)
    local v
    v, i = unpack_u64_dyn_bp(str, i)
    local neg = ((v >> 6) & 1) ~= 0
    v = ((v >> 1) & ~0x3f) | (v & 0x3f)
    if neg then
        v = ~v
    end
    return v, i
end

function pack_i64_dyn(v)
    if warn then
        warn("unpack_u64_dyn_b: renamed to pack_i64_dyn_a")
    end
    return pack_i64_dyn_a(v)
end
function unpack_i64_dyn(str, i)
    if warn then
        warn("unpack_i64_dyn: renamed to unpack_i64_dyn_a")
    end
    return unpack_i64_dyn_a(str, i)
end
function pack_u64_dyn_v2(v)
    if warn then
        warn("pack_u64_dyn_v2: renamed to pack_u64_dyn_b")
    end
    return pack_u64_dyn_b(v)
end
function unpack_u64_dyn_v2(str, i)
    if warn then
        warn("unpack_u64_dyn_v2: renamed to unpack_u64_dyn_b")
    end
    return unpack_u64_dyn_b(str, i)
end
function pack_i64_dyn_v2(v)
    if warn then
        warn("pack_i64_dyn_v2: renamed to pack_i64_dyn_b")
    end
    return pack_i64_dyn_b(v)
end
function unpack_i64_dyn_v2(str, i)
    if warn then
        warn("unpack_i64_dyn_v2: renamed to unpack_i64_dyn_b")
    end
    return unpack_i64_dyn_b(str, i)
end
