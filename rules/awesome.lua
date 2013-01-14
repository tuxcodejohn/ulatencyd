--[[
    Copyright 2010,2011,2012 ulatencyd developers

    This file is part of ulatencyd.

    License: GNU General Public License 3 or later
]]--

AwesomeUI = {
	name = "AwesomeUI",
	re_basename = "awesome",
	check = function(self, proc)
	 local flag = ulatency.new_flag{name="user.ui"}
	 proc:add_flag(flag)
	 proc:set_oom_score(-400)
	 rv = ulatency.filter_rv(ulatency.FILTER_STOP)
	 return rv
	end
}

AwesomeFix = RunnerFix.new("AwesomeFix", {"awesome"})

-- on start we have to fix all processes that have descented from kde
local function cleanup_awesome_mess()
  cleanup_desktop_mess({"awesome"})
  return false
end

ulatency.add_timeout(cleanup_awesome_mess, 1000)
ulatency.register_filter(AwesomeUI)
