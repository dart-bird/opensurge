// -----------------------------------------------------------------------------
// File: profiler.ss
// Description: SurgeScript profiler
// Author: Alexandre Martins <http://opensurge2d.org>
// License: MIT
// -----------------------------------------------------------------------------
using SurgeEngine.Transform;
using SurgeEngine.Vector2;
using SurgeEngine.UI.Text;
using SurgeEngine.Level;

object "Profiler" is "entity", "awake", "special"
{
    uiTimes = spawn("Profiler.UI.Tree");
    uiDensity = spawn("Profiler.UI.Tree");
    uiStats = spawn("Profiler.UI.Tree");
    stats = spawn("Profiler.Stats");
    sortByDesc = spawn("Profiler.UI.Tree.SortByDesc");
    refreshTime = 2.0;
    alternationTime = 8.0;
    counter = 0;

    state "main"
    {
        uiTimes.transform.position = Vector2(0, 0);
        uiDensity.transform.position = Vector2(160, 0);
        uiStats.transform.position = Vector2(310, 0);

        // wait a frame
        state = "update";
    }

    state "update"
    {
        // recompute stats
        cycle = Math.ceil(alternationTime / refreshTime);
        wantAvg = ((counter++) % (2 * cycle)) < cycle;
        wantHist = wantAvg;
        stats.refresh(wantAvg);

        // display stats
        uiStats.updateUI("General", stats.generic, null);

        if(wantAvg)
            uiTimes.updateUI("Time (avg)", stats.timespent, sortByDesc.with(stats.timespent));
        else
            uiTimes.updateUI("Time (sum)", stats.timespent, sortByDesc.with(stats.timespent));

        if(wantHist)
            uiDensity.updateUI("Object histogram", stats.histogram, sortByDesc.with(stats.histogram));
        else
            uiDensity.updateUI("Density tree", stats.density, sortByDesc.with(stats.density));

        // done
        state = "wait";
    }

    state "wait"
    {
        if(timeout(refreshTime))
            state = "update";
    }
}

object "Profiler.Stats"
{
    public readonly generic = {};
    public readonly density = {};
    public readonly histogram = {};
    public readonly timespent = {};
    frames = 0;
    lastRefresh = 0;
    prevObjectCount = 0;
    root = null;
    maxDepth = 16; // increase to inspect more objects (slower)

    state "main"
    {
        // change to inspect a particular object
        root = Level;
        //root = Level.findObject("Default HUD");
        //root = System;

        // count the number of frames
        state = "counting frames";
    }

    state "counting frames"
    {
        frames++;
    }

    fun refresh(wantAverageTimes)
    {
        timeInterval = Math.max(Time.time - lastRefresh, 0.0001);

        density.destroy();
        timespent.destroy();
        generic.destroy();
        computeDensity(root, density = {}, histogram = {}, 1, 1);
        computeTimespent(root, absTimeSpent = {}, count = {}, 1, 1);
        computeGeneric(generic = {}, timeInterval);

        // compute relative times
        timespent = {};
        maxTimeSpent = absTimeSpent[root.__name];
        if(maxTimeSpent > 0) {
            maxAvgTimeSpent = maxTimeSpent / count[root.__name]; // count[root] is usually 1
            keys = absTimeSpent.keys();

            for(i = 0; i < keys.length; i++) {
                key = keys[i];
                val = absTimeSpent[key];
                cnt = count[key];

                if(wantAverageTimes) {
                    avg = val / cnt;
                    avg *= 100 / maxAvgTimeSpent;
                    timespent[key] = avg;
                }
                else {
                    val *= 100 / maxTimeSpent;
                    key = "[" + cnt + "] " + key;
                    timespent[key] = val;
                }
            }
        }

        lastRefresh = Time.time;
    }

    fun computeGeneric(stats, timeInterval)
    {
        // object count
        objectCount = System.objectCount;
        stats["Objects"] = objectCount;

        // spawn rate
        spawnRate = (objectCount - prevObjectCount) / prevObjectCount;
        stats["Spawn rate"] = formatPercentage(spawnRate);
        prevObjectCount = objectCount;

        // profiler object overhead
        objectDelta = System.objectCount - objectCount;
        stats["Overhead"] = objectDelta + " (" + formatPercentage(objectDelta / objectCount) + ")";

        // garbage collection
        garbageSize = System.gc.objectCount;
        stats["Garbage"] = garbageSize + " (" + formatPercentage(garbageSize / objectCount) + ")";

        // fps rate
        stats["FPS Avg."] = formatDecimal(frames / timeInterval);
        stats["FPS Inv."] = frames > 0 ? timeInterval / frames : 0;
        frames = 0;

        // time
        stats["Time"] = Math.floor(Time.time) + "s";

        // top-level entities
        entities = Level.childrenWithTag("entity");
        awakeCount = countAwakeEntities(entities);
        entityCount = entities.length;
        stats["Entities top"] = entityCount;
        stats["Awake"] = awakeCount + " (" + formatPercentage(awakeCount / entityCount) + ")";
    }

    fun computeDensity(obj, tree, histogram, id, depth)
    {
        result = 1;
        key = obj.__name;
        n = obj.__childCount;

        for(i = 0; i < n; i++) {
            child = obj.child(i);
            result += computeDensity(child, tree, histogram, ++id, 1+depth);
        }

        if(depth <= maxDepth) {
            tree[key] += result;
            histogram[key] += 1;
        }

        return result;
    }

    fun computeTimespent(obj, tree, count, id, depth)
    {
        totalTime = 0;
        key = obj.__name;
        n = obj.__childCount;

        for(i = 0; i < n; i++) {
            child = obj.child(i);
            if(child.__active)
                totalTime += computeTimespent(child, tree, count, ++id, 1+depth);
        }
        totalTime += this.__timespent;

        if(depth <= maxDepth) {
            tree[key] += totalTime;
            count[key] += 1;
        }

        return totalTime;
    }

    fun countAwakeEntities(entities)
    {
        count = 0;

        foreach(entity in entities) {
            if(entity.hasTag("awake"))
                count++;
        }

        return count;
    }

    fun formatPercentage(p)
    {
        return formatDecimal(100 * p) + "%";
    }

    fun formatDecimal(num)
    {
        if(typeof(num) !== "number") return num;
        str = (Math.round(num * 100) * 0.01).toString();
        j = str.indexOf(".");
        return j >= 0 ? str.substr(0, j+3) : str;
    }
}

object "Profiler.UI.Tree" is "entity", "detached", "private", "awake"
{
    public readonly transform = Transform();
    text = Text("BoxyBold");

    fun constructor()
    {
        transform.position = Vector2.zero;
        text.zindex = Math.infinity;
        text.maxWidth = 160;
    }

    fun updateUI(title, tree, order)
    {
        str = "<color=ffff00>" + title + "</color>\n";
        keys = tree.keys().sort(order);
        length = keys.length;
        for(i = 0; i < length; i++) {
            key = keys[i];
            value = tree[key];
            str += key + ": " + value + "\n";
        }
        text.text = str;
        keys.destroy();
    }
}

object "Profiler.UI.Tree.SortByDesc"
{
    tree = null;
    fun with(dict) { tree = dict; return this; }
    fun call(a, b) { return f(tree[b]) - f(tree[a]); }
    fun f(s) { return (typeof(s) === "number") ? s : ((j = s.indexOf(" ")) >= 0 ? Number(s.substr(0, j)) : Number(s)); }
}
