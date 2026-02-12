import os
import json
import requests

# The data provided
data = {
	"0":{
		"day":{
			"description":"Sunny",
			"image":"https://openweathermap.org/img/wn/01d@2x.png"
		},
		"night":{
			"description":"Clear",
			"image":"https://openweathermap.org/img/wn/01n@2x.png"
		}
	},
	"1":{
		"day":{
			"description":"Mainly Sunny",
			"image":"https://openweathermap.org/img/wn/01d@2x.png"
		},
		"night":{
			"description":"Mainly Clear",
			"image":"https://openweathermap.org/img/wn/01n@2x.png"
		}
	},
	"2":{
		"day":{
			"description":"Partly Cloudy",
			"image":"https://openweathermap.org/img/wn/02d@2x.png"
		},
		"night":{
			"description":"Partly Cloudy",
			"image":"https://openweathermap.org/img/wn/02n@2x.png"
		}
	},
	"3":{
		"day":{
			"description":"Cloudy",
			"image":"https://openweathermap.org/img/wn/03d@2x.png"
		},
		"night":{
			"description":"Cloudy",
			"image":"https://openweathermap.org/img/wn/03n@2x.png"
		}
	},
	"45":{
		"day":{
			"description":"Foggy",
			"image":"https://openweathermap.org/img/wn/50d@2x.png"
		},
		"night":{
			"description":"Foggy",
			"image":"https://openweathermap.org/img/wn/50n@2x.png"
		}
	},
	"48":{
		"day":{
			"description":"Rime Fog",
			"image":"https://openweathermap.org/img/wn/50d@2x.png"
		},
		"night":{
			"description":"Rime Fog",
			"image":"https://openweathermap.org/img/wn/50n@2x.png"
		}
	},
	"51":{
		"day":{
			"description":"Light Drizzle",
			"image":"https://openweathermap.org/img/wn/09d@2x.png"
		},
		"night":{
			"description":"Light Drizzle",
			"image":"https://openweathermap.org/img/wn/09n@2x.png"
		}
	},
	"53":{
		"day":{
			"description":"Drizzle",
			"image":"https://openweathermap.org/img/wn/09d@2x.png"
		},
		"night":{
			"description":"Drizzle",
			"image":"https://openweathermap.org/img/wn/09n@2x.png"
		}
	},
	"55":{
		"day":{
			"description":"Heavy Drizzle",
			"image":"https://openweathermap.org/img/wn/09d@2x.png"
		},
		"night":{
			"description":"Heavy Drizzle",
			"image":"https://openweathermap.org/img/wn/09n@2x.png"
		}
	},
	"56":{
		"day":{
			"description":"Light Freezing Drizzle",
			"image":"https://openweathermap.org/img/wn/09d@2x.png"
		},
		"night":{
			"description":"Light Freezing Drizzle",
			"image":"https://openweathermap.org/img/wn/09n@2x.png"
		}
	},
	"57":{
		"day":{
			"description":"Freezing Drizzle",
			"image":"https://openweathermap.org/img/wn/09d@2x.png"
		},
		"night":{
			"description":"Freezing Drizzle",
			"image":"https://openweathermap.org/img/wn/09n@2x.png"
		}
	},
	"61":{
		"day":{
			"description":"Light Rain",
			"image":"https://openweathermap.org/img/wn/10d@2x.png"
		},
		"night":{
			"description":"Light Rain",
			"image":"https://openweathermap.org/img/wn/10n@2x.png"
		}
	},
	"63":{
		"day":{
			"description":"Rain",
			"image":"https://openweathermap.org/img/wn/10d@2x.png"
		},
		"night":{
			"description":"Rain",
			"image":"https://openweathermap.org/img/wn/10n@2x.png"
		}
	},
	"65":{
		"day":{
			"description":"Heavy Rain",
			"image":"https://openweathermap.org/img/wn/10d@2x.png"
		},
		"night":{
			"description":"Heavy Rain",
			"image":"https://openweathermap.org/img/wn/10n@2x.png"
		}
	},
	"66":{
		"day":{
			"description":"Light Freezing Rain",
			"image":"https://openweathermap.org/img/wn/10d@2x.png"
		},
		"night":{
			"description":"Light Freezing Rain",
			"image":"https://openweathermap.org/img/wn/10n@2x.png"
		}
	},
	"67":{
		"day":{
			"description":"Freezing Rain",
			"image":"https://openweathermap.org/img/wn/10d@2x.png"
		},
		"night":{
			"description":"Freezing Rain",
			"image":"https://openweathermap.org/img/wn/10n@2x.png"
		}
	},
	"71":{
		"day":{
			"description":"Light Snow",
			"image":"https://openweathermap.org/img/wn/13d@2x.png"
		},
		"night":{
			"description":"Light Snow",
			"image":"https://openweathermap.org/img/wn/13n@2x.png"
		}
	},
	"73":{
		"day":{
			"description":"Snow",
			"image":"https://openweathermap.org/img/wn/13d@2x.png"
		},
		"night":{
			"description":"Snow",
			"image":"https://openweathermap.org/img/wn/13n@2x.png"
		}
	},
	"75":{
		"day":{
			"description":"Heavy Snow",
			"image":"https://openweathermap.org/img/wn/13d@2x.png"
		},
		"night":{
			"description":"Heavy Snow",
			"image":"https://openweathermap.org/img/wn/13n@2x.png"
		}
	},
	"77":{
		"day":{
			"description":"Snow Grains",
			"image":"https://openweathermap.org/img/wn/13d@2x.png"
		},
		"night":{
			"description":"Snow Grains",
			"image":"https://openweathermap.org/img/wn/13n@2x.png"
		}
	},
	"80":{
		"day":{
			"description":"Light Showers",
			"image":"https://openweathermap.org/img/wn/09d@2x.png"
		},
		"night":{
			"description":"Light Showers",
			"image":"https://openweathermap.org/img/wn/09n@2x.png"
		}
	},
	"81":{
		"day":{
			"description":"Showers",
			"image":"https://openweathermap.org/img/wn/09d@2x.png"
		},
		"night":{
			"description":"Showers",
			"image":"https://openweathermap.org/img/wn/09n@2x.png"
		}
	},
	"82":{
		"day":{
			"description":"Heavy Showers",
			"image":"https://openweathermap.org/img/wn/09d@2x.png"
		},
		"night":{
			"description":"Heavy Showers",
			"image":"https://openweathermap.org/img/wn/09n@2x.png"
		}
	},
	"85":{
		"day":{
			"description":"Light Snow Showers",
			"image":"https://openweathermap.org/img/wn/13d@2x.png"
		},
		"night":{
			"description":"Light Snow Showers",
			"image":"https://openweathermap.org/img/wn/13n@2x.png"
		}
	},
	"86":{
		"day":{
			"description":"Snow Showers",
			"image":"https://openweathermap.org/img/wn/13d@2x.png"
		},
		"night":{
			"description":"Snow Showers",
			"image":"https://openweathermap.org/img/wn/13n@2x.png"
		}
	},
	"95":{
		"day":{
			"description":"Thunderstorm",
			"image":"https://openweathermap.org/img/wn/11d@2x.png"
		},
		"night":{
			"description":"Thunderstorm",
			"image":"https://openweathermap.org/img/wn/11n@2x.png"
		}
	},
	"96":{
		"day":{
			"description":"Light Thunderstorms With Hail",
			"image":"https://openweathermap.org/img/wn/11d@2x.png"
		},
		"night":{
			"description":"Light Thunderstorms With Hail",
			"image":"https://openweathermap.org/img/wn/11n@2x.png"
		}
	},
	"99":{
		"day":{
			"description":"Thunderstorm With Hail",
			"image":"https://openweathermap.org/img/wn/11d@2x.png"
		},
		"night":{
			"description":"Thunderstorm With Hail",
			"image":"https://openweathermap.org/img/wn/11n@2x.png"
		}
	}
}

import os
import json
import requests
from PIL import Image
import io

def download_weather_icons(json_data):
    # 1. Create a directory for the icons if it doesn't exist
    folder_name = "weather_icons_resized"
    if not os.path.exists(folder_name):
        os.makedirs(folder_name)
    
    # 2. Extract all unique image URLs to avoid redundant downloads
    urls = set()
    for code in json_data.values():
        urls.add(code['day']['image'])
        urls.add(code['night']['image'])
    
    print(f"Found {len(urls)} unique icons to download.")
    print("Set of icons:")
    print(sorted([url.split("/")[-1] for url in urls]))

    # # 3. Download each icon
    # for url in urls:
    #     # Extract filename (e.g., 01d@2x.png) from the URL
    #     filename = url.split("/")[-1]
    #     filepath = os.path.join(folder_name, filename)
        
    #     try:
    #         response = requests.get(url)
    #         response.raise_for_status() # Check for errors
            
    #         img_data = response.content
    #         with Image.open(io.BytesIO(img_data)) as img:
    #             img_resized = img.resize((50, 50), Image.Resampling.LANCZOS)
    #             img_resized.save(filepath)
                
    #         print(f"Successfully downloaded and resized: {filename}")
            
    #     except Exception as e:
    #         print(f"Failed to download {url}: {e}")

    # 4. Generate and print maps
    day_map = {}
    night_map = {}
    
    for code, info in json_data.items():
        day_map[code] = info['day']['image'].split("/")[-1]
        night_map[code] = info['night']['image'].split("/")[-1]
        
    print("\nDay Map (Code -> Icon):")
    print(json.dumps(day_map, indent=4))
    
    print("\nNight Map (Code -> Icon):")
    print(json.dumps(night_map, indent=4))

    # 5. Generate C arrays
    print("\n// C Code for Weather Icons")
    
    # 5. Generate C arrays
    print("\n// C Code for Weather Icons")
    
    # Helper to sanitize filenames to variable names
    def get_var_name(filename):
        # filename example: 01d@2x.png
        # desired: ui_img_01d_2x_png_data
        name = filename.replace('@', '_').replace('.', '_')
        return f"ui_img_{name}_data"

    # Collect all unique icons
    all_icons = set(day_map.values()) | set(night_map.values())
    sorted_icons = sorted(list(all_icons))

    print("// Extern declarations")
    for icon in sorted_icons:
        var_name = get_var_name(icon)
        print(f"extern const LV_ATTRIBUTE_MEM_ALIGN uint8_t {var_name}[];")
    print("")
    
    # Separate arrays for day and night unique icons
    unique_day = sorted(list(set(day_map.values())))
    unique_night = sorted(list(set(night_map.values())))

    print("// Array of unique day icons")
    print(f"const uint8_t * weather_icons_list_day[{len(unique_day)}] = {{")
    for icon in unique_day:
        print(f"    {get_var_name(icon)},")
    print("};")
    print("")

    print("// Array of unique night icons")
    print(f"const uint8_t * weather_icons_list_night[{len(unique_night)}] = {{")
    for icon in unique_night:
        print(f"    {get_var_name(icon)},")
    print("};")
    print("")

    def print_lookup_function_ranges(func_name, map_data):
        print(f"const uint8_t* {func_name}(int code) {{")
        
        # Invert map to group codes by icon
        icon_to_codes = {}
        for code, icon in map_data.items():
            if icon not in icon_to_codes:
                icon_to_codes[icon] = []
            icon_to_codes[icon].append(int(code))
            
        first_if = True
        
        for icon in sorted(icon_to_codes.keys()):
            codes = sorted(icon_to_codes[icon])
            var_name = get_var_name(icon)
            
            # Find ranges in codes
            ranges = []
            if not codes: continue
            
            start = codes[0]
            prev = codes[0]
            
            for code in codes[1:]:
                if code == prev + 1:
                    prev = code
                else:
                    ranges.append((start, prev))
                    start = code
                    prev = code
            ranges.append((start, prev))
            
            # Build condition string
            conditions = []
            for r in ranges:
                if r[0] == r[1]:
                    conditions.append(f"code == {r[0]}")
                else:
                    conditions.append(f"(code >= {r[0]} && code <= {r[1]})")
            
            condition_str = " || ".join(conditions)
            
            if first_if:
                print(f"    if ({condition_str}) {{")
                first_if = False
            else:
                print(f"    else if ({condition_str}) {{")
            print(f"        return {var_name};")
            print("    }")
            
        print("    return NULL;")
        print("}")

    print_lookup_function_ranges("get_weather_icon_day", day_map)
    print("")
    print_lookup_function_ranges("get_weather_icon_night", night_map)

if __name__ == "__main__":
    # If your JSON is in a file, you'd load it like this:
    # with open('weather.json', 'r') as f:
    #     data = json.load(f)
    
    download_weather_icons(data)