-- This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
print("testing native code generation")

assert((function(x, y)
  -- trigger a linear sequence
  local t1 = x + 2
  local t2 = x - 7

  local a = x * 10
  local b = a + 1
  a = y -- update 'a' version
  local t = {} -- call to allocate table forces a spill
  local c = x * 10
  return c, b, t, t1, t2
end)(5, 10) == 50)

assert((function(x)
  local oops -- split to prevent inlining
  function oops()
  end

  -- x is checked to be a number here; we can not execute a reentry from oops() because optimizer assumes this holds until return
  local y = math.abs(x)
  oops()
  return y * x
end)("42") == 1764)

local function fuzzfail1(...)
  repeat
    _ = nil
  until not {}
  for _ in ... do
    for l0=_,_ do
    end
    return
  end
end

local function fuzzfail2()
  local _
  do
    repeat
      _ = typeof(_),{_=_,}
      _ = _(_._)
    until _
  end
end

assert(pcall(fuzzfail2) == false)

local function fuzzfail3()
  function _(...)
    _({_,_,true,},{...,},_,not _)
  end
  _()
end

assert(pcall(fuzzfail3) == false)

local function fuzzfail4()
  local _ = setmetatable({},setmetatable({_=_,},_))
  return _(_:_())
end

assert(pcall(fuzzfail4) == false)

local function fuzzfail5()
  local _ = bit32.band
  _(_(_,0),_)
  _(_,_)
end

assert(pcall(fuzzfail5) == false)

local function fuzzfail6(_)
  return bit32.extract(_,671088640,_)
end

assert(pcall(fuzzfail6, 1) == false)

local function fuzzfail7(_)
  return bit32.extract(_,_,671088640)
end

assert(pcall(fuzzfail7, 1) == false)

local function fuzzfail8(...)
  local _ = _,_
  _.n0,_,_,_,_,_,_,_,_._,_,_,_[...],_,_,_ = nil
  _,n0,_,_,_,_,_,_,_,_,l0,_,_,_,_ = nil
  function _()
  end
  _._,_,_,_,_,_,_,_,_,_,_[...],_,n0[l0],_ = nil
  _[...],_,_,_,_,_,_,_,_()[_],_,_,_,_,_ = _(),...
end

assert(pcall(fuzzfail8) == false)

local function fuzzfail9()
  local _ = bit32.bor
  local x = _(_(_,_),_(_,_),_(-16834560,_),_(_(- _,-2130706432)),- _),_(_(_,_),_(-16834560,-2130706432))
end

assert(pcall(fuzzfail9) == false)

local function fuzzfail10()
  local _
  _ = false,if _ then _ else _
  _ = not _
  l0,_[l0] = not _
end

assert(pcall(fuzzfail10) == false)

local function fuzzfail11(x, ...)
  return bit32.arshift(bit32.bnot(x),(...))
end

assert(fuzzfail11(0xffff0000, 8) == 0xff)

local function fuzzfail12()
  _,_,_,_,_,_,_,_ = not _, not _, not _, not _, not _, not _, not _, not _
end

assert(pcall(fuzzfail12) == true)

local function fuzzfail13()
  _,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_ = not _, not _, not _, not _, not _, not _, not _, not _, not _, not _, not _, not _, not _, not _, not _, not _
end

assert(pcall(fuzzfail13) == true)

local function arraySizeInv1()
  local t = {1, 2, nil, nil, nil, nil, nil, nil, nil, true}

  table.insert(t, 3)

  return t[10]
end

assert(arraySizeInv1() == true)

local function arraySizeInv2()
  local t = {1, 2, nil, nil, nil, nil, nil, nil, nil, true}

  local u = {a = t}
  table.insert(u.a, 3) -- aliased modifiction of 't' register through other value

  return t[10]
end

assert(arraySizeInv2() == true)

local function nilInvalidatesSlot()
  local function tabs()
    local t = { x=1, y=2, z=3 }
    setmetatable(t, { __index = function(t, k) return 42 end })
    return t, t
  end

  local t1, t2 = tabs()

  for i=1,2 do
    local a = t1.x
    t2.x = nil
    local b = t1.x
    t2.x = 1
    assert(a == 1 and b == 42)
  end
end

nilInvalidatesSlot()

return('OK')
