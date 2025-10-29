<?php
function add64($a, $b)
{
    $mask = -1;
    while ($b != 0)
    {
        $carry = ($a & $b) << 1;
        $a = ($a ^ $b) & $mask;
        $b = $carry & $mask;
    }
    return $a;
}

function cmp64($a, $b)
{
    $a_hi = ($a >> 32) & 0xffffffff;
    $b_hi = ($b >> 32) & 0xffffffff;
    if ($a_hi > $b_hi)
    {
        return 1;
    }
    if ($a_hi < $b_hi)
    {
        return -1;
    }
    $a_lo = $a & 0xffffffff;
    $b_lo = $b & 0xffffffff;
    return $a_lo <=> $b_lo;
}

function get_bias($byte_length)
{
    $bias = 0;
    while (--$byte_length)
    {
        $bias += 1;
        $bias <<= 7;
    }
    return $bias;
}

function pack_u64_dyn($v)
{
    if (is_float($v))
    {
        throw new Exception("Cannot encode a float as u64");
    }
    $out = "";
    for ($i = 0; $i != 8; ++$i)
    {
        $cur = $v & 0x7f;
        $v >>= 7;
        if ($v != 0)
        {
            $out .= chr($cur | 0x80);
        }
        else
        {
            $out .= chr($cur);
            return $out;
        }
    }
    $out .= chr($v);
    return $out;
}

function unpack_u64_dyn($str, &$offset = 0)
{
    $len = strlen($str);
    $v = 0;
    $bits = 0;
    $i = $offset;
    while ($i < $len && $i - $offset < 8)
    {
        $b = ord($str[$i]);
        $i++;
        $v = add64($v, ($b & 0x7f) << $bits);
        if (($b & 0x80) == 0)
        {
            $offset = $i;
            return $v;
        }
        $bits += 7;
    }
    if ($i >= $len)
    {
        throw new Exception("Insufficient data");
    }
    $b = ord($str[$i]);
    $v = add64($v, $b << 56);
    $i++;
    $offset = $i;
    return $v;
}

function pack_u64_dyn_b($v)
{
    if (is_float($v))
    {
        throw new Exception("Cannot encode a float as u64");
    }
    $out = "";
    for ($i = 0; $i != 8; ++$i)
    {
        $cur = $v & 0x7f;
        $v >>= 7;
        if ($v != 0)
        {
            $out .= chr($cur | 0x80);
            $v -= 1; // bias
        }
        else
        {
            $out .= chr($cur);
            return $out;
        }
    }
    $out .= chr($v);
    return $out;
}

function unpack_u64_dyn_b($str, &$offset = 0)
{
    $len = strlen($str);
    $v = 0;
    $bits = 0;
    $i = $offset;
    $bias = 0;
    while (true)
    {
        if ($i >= $len)
        {
            throw new Exception("Insufficient data");
        }
        $b = ord($str[$i]);
        $i++;
        if ($i - $offset == 9)
        {
            $v = add64($v, $b << 56);
            break;
        }
        $v = add64($v, ($b & 0x7f) << $bits);
        if (($b & 0x80) == 0)
        {
            break;
        }
        $bits += 7;
        $bias = add64($bias, 1 << $bits);
    }
    $offset = $i;
    if (cmp64($v, add64(-1, -$bias)) > 0)
    {
        throw new Exception("Invalid data");
    }
    return add64($v, $bias);
}

function pack_u64_dyn_p($v)
{
    if (is_float($v))
    {
        throw new Exception("Cannot encode a float as u64");
    }

    $byte_length = 1;
    if (($v >> 7) != 0) { $byte_length += 1; }
    if (($v >> 14) != 0) { $byte_length += 1; }
    if (($v >> 21) != 0) { $byte_length += 1; }
    if (($v >> 28) != 0) { $byte_length += 1; }
    if (($v >> 35) != 0) { $byte_length += 1; }
    if (($v >> 42) != 0) { $byte_length += 1; }
    if (($v >> 49) != 0) { $byte_length += 1; }
    if (($v >> 56) != 0) { $byte_length += 1; }

    $first_byte_value_bits = $byte_length < 8 ? 8 - $byte_length : 0;
    $first_byte_prefix_bits = $byte_length - 1;

    $first_byte = ((0xff << (8 - $first_byte_prefix_bits)) & 0xff) | ($v & ((1 << $first_byte_value_bits) - 1));
    $v >>= $first_byte_value_bits;

    $out = chr($first_byte);
    for ($idx = 1; $idx < $byte_length; ++$idx)
    {
        $out .= chr(($v >> (($idx - 1) * 8)) & 0xff);
    }

    return $out;
}

function unpack_u64_dyn_p($str, &$offset = 0)
{
    $len = strlen($str);
    if ($offset >= $len)
    {
        throw new Exception("Insufficient data");
    }

    $first_byte = ord($str[$offset]);

    $prefix_bits = 0;
    while ($prefix_bits < 8 && ($first_byte & (0x80 >> $prefix_bits)) != 0)
    {
        $prefix_bits += 1;
    }

    $byte_length = $prefix_bits + 1;
    $first_byte_value_bits = $byte_length < 8 ? 8 - $byte_length : 0;

    if ($offset + $byte_length > $len)
    {
        throw new Exception("Insufficient data");
    }

    $v = 0;
    for ($idx = 1; $idx != $byte_length; ++$idx)
    {
        $b = ord($str[$offset + $idx]);
        $v |= $b << (($idx - 1) * 8);
    }

    $v <<= $first_byte_value_bits;
    $v |= $first_byte & ((1 << $first_byte_value_bits) - 1);

    $offset += $byte_length;
    return $v;
}

function pack_u64_dyn_bp($v)
{
    if (is_float($v))
    {
        throw new Exception("Cannot encode a float as u64");
    }

    $byte_length = 1;
    if (cmp64($v, 0x80) >= 0) { $byte_length += 1; }
    if (cmp64($v, 0x4080) >= 0) { $byte_length += 1; }
    if (cmp64($v, 0x204080) >= 0) { $byte_length += 1; }
    if (cmp64($v, 0x10204080) >= 0) { $byte_length += 1; }
    if (cmp64($v, 0x810204080) >= 0) { $byte_length += 1; }
    if (cmp64($v, 0x40810204080) >= 0) { $byte_length += 1; }
    if (cmp64($v, 0x2040810204080) >= 0) { $byte_length += 1; }
    if (cmp64($v, 0x102040810204080) >= 0) { $byte_length += 1; }

    $first_byte_value_bits = $byte_length < 8 ? 8 - $byte_length : 0;
    $first_byte_prefix_bits = $byte_length - 1;

    $v = add64($v, -get_bias($byte_length));

    $first_byte = ((0xff << (8 - $first_byte_prefix_bits)) & 0xff) | ($v & ((1 << $first_byte_value_bits) - 1));
    $v >>= $first_byte_value_bits;

    $out = chr($first_byte);
    for ($idx = 1; $idx < $byte_length; ++$idx)
    {
        $out .= chr(($v >> (($idx - 1) * 8)) & 0xff);
    }

    return $out;
}

function unpack_u64_dyn_bp($str, &$offset = 0)
{
    $len = strlen($str);
    if ($offset >= $len)
    {
        throw new Exception("Insufficient data");
    }

    $first_byte = ord($str[$offset]);

    $prefix_bits = 0;
    while ($prefix_bits < 8 && ($first_byte & (0x80 >> $prefix_bits)) != 0)
    {
        $prefix_bits += 1;
    }

    $byte_length = $prefix_bits + 1;
    $first_byte_value_bits = $byte_length < 8 ? 8 - $byte_length : 0;

    if ($offset + $byte_length > $len)
    {
        throw new Exception("Insufficient data");
    }

    $v = 0;
    for ($idx = 1; $idx != $byte_length; ++$idx)
    {
        $b = ord($str[$offset + $idx]);
        $v |= $b << (($idx - 1) * 8);
    }

    $v <<= $first_byte_value_bits;
    $v |= $first_byte & ((1 << $first_byte_value_bits) - 1);

    $bias = get_bias($byte_length);
    if (cmp64($v, add64(-1, -$bias)) > 0)
    {
        throw new Exception("Invalid data");
    }

    $offset += $byte_length;
    return add64($v, $bias);
}

function pack_i64_dyn_a($v)
{
    if (is_float($v))
    {
        throw new Exception("Cannot encode a float as i64");
    }
    $neg = ($v >> 63) & 1;
    if ($v < 0)
    {
        $u = add64(~$v, 1) & ~(-1 << 63);
    }
    else
    {
        $u = $v;
    }
    return pack_u64_dyn(($neg << 6) | (($u & ~0x3f) << 1) | ($u & 0x3f));
}

function unpack_i64_dyn_a($str, &$offset = 0)
{
    $v = unpack_u64_dyn($str, $offset);
    $neg = (($v >> 6) & 1) != 0;
    $upper = $v & ~0x7f;
    $upper = ($upper >> 1) & ~(-1 << 63);
    $v = $upper | ($v & 0x3f);
    if ($neg)
    {
        $v = (~($v - 1)) | (1 << 63);
    }
    return $v;
}

function pack_i64_dyn_b($v)
{
    if (is_float($v))
    {
        throw new Exception("Cannot encode a float as i64");
    }
    $neg = ($v >> 63) & 1;
    if ($v < 0)
    {
        $v = ~$v;
    }
    return pack_u64_dyn_b(($neg << 6) | (($v & ~0x3f) << 1) | ($v & 0x3f));
}

function unpack_i64_dyn_b($str, &$offset = 0)
{
    $v = unpack_u64_dyn_b($str, $offset);
    $neg = (($v >> 6) & 1) != 0;
    $upper = $v & ~0x7f;
    $upper = ($upper >> 1) & ~(-1 << 63);
    $v = $upper | ($v & 0x3f);
    if ($neg)
    {
        $v = ~$v;
    }
    return $v;
}

function pack_i64_dyn_bp($v)
{
    if (is_float($v))
    {
        throw new Exception("Cannot encode a float as i64");
    }
    $neg = ($v >> 63) & 1;
    if ($v < 0)
    {
        $v = ~$v;
    }
    return pack_u64_dyn_bp(($neg << 6) | (($v & ~0x3f) << 1) | ($v & 0x3f));
}

function unpack_i64_dyn_bp($str, &$offset = 0)
{
    $v = unpack_u64_dyn_bp($str, $offset);
    $neg = (($v >> 6) & 1) != 0;
    $upper = $v & ~0x7f;
    $upper = ($upper >> 1) & ~(-1 << 63);
    $v = $upper | ($v & 0x3f);
    if ($neg)
    {
        $v = ~$v;
    }
    return $v;
}

function warn_deprecated($old, $new)
{
    if (function_exists('trigger_error'))
    {
        @trigger_error("$old: renamed to $new", E_USER_DEPRECATED);
    }
}

function pack_u64_dyn_v2($v)
{
    warn_deprecated('pack_u64_dyn_v2', 'pack_u64_dyn_b');
    return pack_u64_dyn_b($v);
}

function unpack_u64_dyn_v2($str, &$offset = 0)
{
    warn_deprecated('unpack_u64_dyn_v2', 'unpack_u64_dyn_b');
    return unpack_u64_dyn_b($str, $offset);
}

function pack_i64_dyn($v)
{
    warn_deprecated('pack_i64_dyn', 'pack_i64_dyn_a');
    return pack_i64_dyn_a($v);
}

function unpack_i64_dyn($str, &$offset = 0)
{
    warn_deprecated('unpack_i64_dyn', 'unpack_i64_dyn_a');
    return unpack_i64_dyn_a($str, $offset);
}

function pack_i64_dyn_v2($v)
{
    warn_deprecated('pack_i64_dyn_v2', 'pack_i64_dyn_b');
    return pack_i64_dyn_b($v);
}

function unpack_i64_dyn_v2($str, &$offset = 0)
{
    warn_deprecated('unpack_i64_dyn_v2', 'unpack_i64_dyn_b');
    return unpack_i64_dyn_b($str, $offset);
}
