------------------------------------------------------------------------
--
-- this is a implementation of the classic eliza in pure lua.
-- this code is based on Kein-Hong Man's Eliza for Scite 
-- ( http://lua-users.org/wiki/SciteElizaClassic)
-- sligtly modified to be used as a lua module
-- 
-- This program is hereby placed into PUBLIC DOMAIN
-- startx <startx@plentyfact.org> dec 2010
------------------------------------------------------------------------

function answer(text)
  local response = ""
  local user = string.upper(text)
  local userOrig = user

  -- randomly selected replies if no keywords
  local randReplies = {
    "What does that suggest to you?",
    "I see...",
    "I'm not sure I understand you fully.",
    "Can you elaborate on that?",
    "That is quite interesting!",
    "That's so... please continue...",
    "I understand...",
    "Well, well... do go on",
    "Why are you saying that?",
    "Please explain the background to that remark...",
    "Could you say that again, in a different way?",
  }
   

  -- keywords, replies
  local replies = {
    [" CAN YOU"] = "Perhaps you would like to be able to",
    [" DO YOU"] = "Yes, I",
    [" CAN I"] = "Perhaps you don't want to be able to",
    [" YOU ARE"] = "What makes you think I am",
    [" YOU'RE"] = "What is your reaction to me being",
    [" I DON'T"] = "Why don't you",
    [" I FEEL"] = "Tell me more about feeling",
    [" WHY DON'T YOU"] = "Why would you want me to",
    [" WHY CAN'T I"] = "What makes you think you should be able to",
    [" ARE YOU"] = "Why are you interested in whether or not I am",
    [" I CAN'T"] = "How do you know you can't",
    [" SEX"] = "I feel you should discuss this with a human.",
    [" I AM"] = "How long have you been",
    [" I'M"] = "Why are you telling me you're",
    [" I WANT"] = "Why do you want",
    [" WHAT"] = "What do you think?",
    [" HOW"] = "What answer would please you the most?",
    [" WHO"] = "How often do you think of such questions?",
    [" WHERE"] = "Why did you think of that?",
    [" WHEN"] = "What would your best friend say to that question?",
    [" WHY"] = "What is it that you really want to know?",
    [" PERHAPS"] = "You're not very firm on that!",
    [" DRINK"] = "Moderation in all things should ne the rule.",
    [" SORRY"] = "Why are you apologizing?",
    [" DREAMS"] = "Why did you bring up the subject of dreams?",
    [" I LIKE"] = "Is it good that you like",
    [" MAYBE"] = "Aren't you being a bit tentative?",
    [" NO"] = "Why are you being negative?",
    [" YOUR"] = "Why are you concerned about my",
    [" ALWAYS"] = "Can you think of a specific example?",
    [" THINK"] = "Do you doubt",
    [" YES"] = "You seem quite certain. Why is this so?",
    [" FRIEND"] = "Why do you bring up the subject of friends?",
    [" COMPUTER"] = "Why do you mention computers?",
    [" AM I"] = "You are ARE",
    [ "HI" ] = "How are you today.. What would you like to discuss?",
  }

  -- conjugate
  
   local conjugate = {                              
    [ "are" ]  = "am",
    [ "am" ]   = "are",
    [ "were" ] = "was",
    [ "was" ]  = "were",
    [ "I" ]    = "you",    
    [ "me" ]   = "you",    
    [ "you" ]  = "me",
    [ "my" ]   = "your",    
    [ "your" ] = "my",
    [ "mine" ] = "your's",    
    [ "your's" ] = "mine",    
    [ "I'm" ] = "you're",
    [ "you're" ] = "I'm",    
    [ "I've" ]   = "you've",
    [ "you've" ] = "I've",
    [ "I'll" ]   = "you'll",
    [ "you'll" ] = "I'll",
    [ "myself" ] = "yourself",
    [ "yourself" ] = "myself",
    [ "me am" ]  = "I am",
    [ "am me" ]  = "am I",
    [ "me can" ] = "I can",
    [ "can me" ] = "can I",
    [ "me have" ] = "I have",
    [ "me will" ] = "I will",
    [ "will me" ] = "will I",
  }


  local function replyRandomly()
    response = randReplies[tonumber(reqdata['r'])].."\n"
  end

  -- find keyword, phrase
  local function processInput()
    for keyword, reply in pairs(replies) do
      local d, e = string.find(user, keyword, 1, 1)
      if d then
        -- process keywords
        response = response..reply.." "
        if string.byte(string.sub(reply, -1)) < 65 then -- "A"
          response = response.."\n"; return
        end
        local h = string.len(user) - (d + string.len(keyword))
        if h > 0 then
          user = string.sub(user, -h)
        end
        for cFrom, cTo in pairs(conjugate) do
          local f, g = string.find(user, cFrom, 1, 1)
          if f then
            local j = string.sub(user, 1, f - 1).." "..cTo
            local z = string.len(user) - (f - 1) - string.len(cTo)
            response = response..j.."\n"
            if z > 2 then
              local l = string.sub(user, -(z - 2))
              if not string.find(userOrig, l) then return end
            end
            if z > 2 then response = response..string.sub(user, -(z - 2)).."\n" end
            if z < 2 then response = response.."\n" end
            return
          end--if f
        end--for
        response = response..user.."\n"
        return
      end--if d
    end--for
    replyRandomly()
    return
  end

  -- main()
  -- accept user input
  if string.sub(user, 1, 3) == "BYE" then
    response = "Bye, bye for now.\nSee you again some time.\n"
    return response
  end
  if string.sub(user, 1, 7) == "because" then
    user = string.sub(user, 8)
  end
  user = " "..user.." "
  -- process input, print reply
  processInput()
  response = response.."\n"
  return response
end

--print(json.stringify(answer(input)))
if reqdata['p'] == "case_start" then
  print("Hello, I am Eliza")
elseif  reqdata['p'] == "case_noinput" then
  print ("Are We going to Chat?")
elseif reqdata['p'] == "case_noinput1" then
  print ("I can't help you without a dialog!")
elseif reqdata['p'] == "case_empty" then
  print ("I can't help, if you will not chat with me!")
else
  print(answer(reqdata['p']))
end

