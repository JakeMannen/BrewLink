---
name: brewing-recipe-schemas
description: Comprehensive agent skill for parsing, validating, transforming, and bidirectional crosswalk conversions between BeerXML 1.0 and BeerJSON specification standards.
tags:
  - beerxml
  - beerjson
  - homebrewing
  - craft-brewing
  - recipe-format
  - json-schema
  - xml
triggers:
  - parse beerxml
  - parse beerjson
  - convert beerxml to beerjson
  - convert beerjson to beerxml
  - validate brewing recipe
  - beer recipe converter
  - beerjson schema reference
  - beerxml schema reference
---

# BeerXML 1.0 & BeerJSON Schema Specialist Skill

This skill equips autonomous agents and brewing software systems with complete operational knowledge, conversion algorithms, validation rules, and schema mappings for the two primary digital interchange standards in brewing: **BeerXML 1.0** and **BeerJSON (v2.x)**.

---

## 1. Domain Background & Comparative Architecture

### 1.1 Evolution of Brewing Interchange Standards
* **BeerXML 1.0 (2003–2004):** Designed collaboratively by Brad Smith (*BeerSmith*), Drew Avis (*Strangebrew*), Michael Taylor (*SUDS*), Andrew Perron (*DrewBrew*), and David Johnson (*QBrew*). BeerXML was built as an open XML-based dialect to allow cross-software recipe interchange. It uses flat records, upper-case tag names, and fixed implicit SI units.
* **BeerJSON (v1.0 - v2.x):** Developed by an international open-source community to modernize brewing recipe storage. BeerJSON replaces XML with JSON Schema (Draft-07), introduces explicit composite unit types (value + unit objects), models process stages modularly (multi-step boils, complex whirlpooling, granular fermentation profiles), and unifies microbiological agents under `cultures`.

### 1.2 Architectural Comparison Matrix

| Architectural Feature | BeerXML 1.0 | BeerJSON (v2.x) |
| :--- | :--- | :--- |
| **Data Format** | XML 1.0 (recommended `ISO-8859-1` or `UTF-8`) | JSON (strict `UTF-8`) |
| **Formal Specification** | HTML specification document, unofficial DTD/XSD | Formal JSON Schema (`draft-07`) |
| **Root Encapsulation** | Plural record-set containers (`<RECIPES>`, `<HOPS>`) | Single root object: `{"beerjson": { ... }}` |
| **Unit Philosophy** | **Strictly Implicit SI** (kg, L, °C, min, kPa, SG) | **Explicit Objects** (`{"value": 5.0, "unit": "kg"}`) |
| **Yeast / Microbes** | Flat `<YEAST>` entity | Polymorphic `culture_information` / `culture_additions` |
| **Hop Additions** | Coarse `<USE>` tag (`Boil`, `Dry Hop`, `Mash`, etc.) | Detailed `timing` object (stage, time, duration, temp) |
| **Mashing** | Standard infusion/decoction/temperature steps | Ordered `mash_steps` with water-to-grain ratios & infusion |
| **Fermentation** | Flat fields (`PRIMARY_AGE`, `SECONDARY_AGE`, etc.) | Sequential `fermentation_steps` with temp & pressure ramps |
| **Water Chemistry** | Static source ion profile | Source water profile + explicit mineral/acid salt additions |
| **Extensibility** | Unrecognized XML tags ignored | Custom properties allowed / extension objects |

---

## 2. BeerXML 1.0 Specification Reference

### 2.1 File & Syntax Conventions
1. **File Extension:** `.xml` (e.g., `recipe.xml`, `hops.xml`).
2. **Declaration:** Standard XML prolog:
   ```xml
   <?xml version="1.0" encoding="ISO-8859-1"?>
   ```
   *(Note: UTF-8 is widely accepted in modern implementations, but parsers must handle ISO-8859-1).*
3. **Tag Names:** Tag names are strictly uppercase (`<NAME>`, `<AMOUNT>`, `<VERSION>`).
4. **Attributes vs Elements:** BeerXML forbids XML attributes for data fields. All values must be child tags:
   * **Allowed:** `<HOP><NAME>Cascade</NAME></HOP>`
   * **Forbidden:** `<HOP NAME="Cascade" />`
5. **Entity Escaping:** Standard XML entities must be properly handled:
   * `&` -> `&amp;`
   * `<` -> `&lt;`
   * `>` -> `&gt;`
   * `"` -> `&quot;`
   * `'` -> `&apos;`

### 2.2 Implicit Unit Standards
In BeerXML 1.0, units are **never explicitly declared** in data fields. Software must enforce these exact dimensions:

* **Weight / Mass:** Kilograms (`kg`)
* **Volume:** Liters (`l`)
* **Temperature:** Degrees Celsius (`°C`)
* **Time:**
  * Minutes (`min`) for boil, mash steps, additions.
  * Days for fermentation stages (`PRIMARY_AGE`, `SECONDARY_AGE`).
* **Specific Gravity:** Relative density to water at calibration temp (e.g., `1.054`).
* **Pressure:** Kilopascals (`kPa`).
* **Percentages:** Represented as `0 - 100` (e.g., `75.5%` is formatted as `75.5`, NOT `0.755`).

### 2.3 Record Sets and Primary Entities

#### Record Set Wrapper Tags
* `<RECIPES>`: Encloses one or more `<RECIPE>` records.
* `<HOPS>`: Encloses one or more standalone `<HOP>` records.
* `<FERMENTABLES>`: Encloses one or more standalone `<FERMENTABLE>` records.
* `<YEASTS>`: Encloses one or more standalone `<YEAST>` records.
* `<MISCS>`: Encloses one or more standalone `<MISC>` records.
* `<WATERS>`: Encloses one or more standalone `<WATER>` records.
* `<STYLES>`: Encloses one or more standalone `<STYLE>` records.
* `<MASHS>`: Encloses one or more standalone `<MASH>` records.
* `<EQUIPMENTS>`: Encloses one or more standalone `<EQUIPMENT>` records.

---

### 2.4 Detailed Record Definitions

#### 1. `<RECIPE>` Record
| Tag | Required | Type | Description |
| :--- | :--- | :--- | :--- |
| `NAME` | **Yes** | Text | Recipe title. |
| `VERSION` | **Yes** | Integer | Must be `1`. |
| `TYPE` | **Yes** | Enum | `All Grain`, `Extract`, or `Partial Mash`. |
| `BREWER` | No | Text | Brewer name. |
| `BATCH_SIZE` | **Yes** | Float (L) | Target volume into fermenter. |
| `BOIL_SIZE` | **Yes** | Float (L) | Pre-boil kettle volume. |
| `BOIL_TIME` | **Yes** | Float (min)| Standard total boil duration. |
| `EFFICIENCY` | No | Float (%) | Expected brewhouse efficiency (required for All Grain). |
| `HOPS` | No | Record Set | Container of `<HOP>` records. |
| `FERMENTABLES` | No | Record Set | Container of `<FERMENTABLE>` records. |
| `YEASTS` | No | Record Set | Container of `<YEAST>` records. |
| `MISCS` | No | Record Set | Container of `<MISC>` records. |
| `STYLE` | No | Record | Embedded `<STYLE>` record. |
| `EQUIPMENT` | No | Record | Embedded `<EQUIPMENT>` record. |
| `MASH` | No | Record | Embedded `<MASH>` record. |
| `PRIMARY_AGE` | No | Float (days)| Duration of primary fermentation. |
| `PRIMARY_TEMP`| No | Float (°C) | Temperature of primary fermentation. |
| `SECONDARY_AGE`| No | Float (days)| Duration of secondary fermentation. |
| `SECONDARY_TEMP`| No | Float (°C) | Temperature of secondary fermentation. |
| `OG` | No | Float (SG) | Calculated/measured Original Gravity. |
| `FG` | No | Float (SG) | Calculated/measured Final Gravity. |
| `IBU` | No | Float | Calculated bitterness in IBUs. |

#### 2. `<HOP>` Record
| Tag | Required | Type | Description |
| :--- | :--- | :--- | :--- |
| `NAME` | **Yes** | Text | Hop cultivar name (e.g., `Cascade`, `Mosaic`). |
| `VERSION` | **Yes** | Integer | Must be `1`. |
| `ALPHA` | **Yes** | Float (%) | Alpha acid percentage (e.g., `12.5` = 12.5%). |
| `AMOUNT` | **Yes** | Float (kg) | Weight added in kilograms (e.g., `0.02835` = 1 oz). |
| `USE` | **Yes** | Enum | `Boil`, `Dry Hop`, `Mash`, `First Wort`, `Aroma`. |
| `TIME` | **Yes** | Float | Time in minutes (for Boil, Mash, First Wort, Aroma) or days (Dry Hop). |
| `NOTES` | No | Text | Description / aroma profile. |
| `TYPE` | No | Enum | `Bittering`, `Aroma`, `Both`. |
| `FORM` | No | Enum | `Pellet`, `Plug`, `Leaf`. |
| `BETA` | No | Float (%) | Beta acid percentage. |
| `HSI` | No | Float (%) | Hop Stability Index (% alpha lost in 6 mo). |

#### 3. `<FERMENTABLE>` Record
| Tag | Required | Type | Description |
| :--- | :--- | :--- | :--- |
| `NAME` | **Yes** | Text | Malt, grain, sugar, or extract name. |
| `VERSION` | **Yes** | Integer | Must be `1`. |
| `TYPE` | **Yes** | Enum | `Grain`, `Sugar`, `Extract`, `Dry Extract`, `Adjunct`. |
| `AMOUNT` | **Yes** | Float (kg) | Weight in kilograms. |
| `YIELD` | **Yes** | Float (%) | Fine-grind dry basis (FGDB) yield relative to sucrose. |
| `COLOR` | **Yes** | Float (°L) | Lovibond / SRM color rating. |
| `ADD_AFTER_BOIL` | No | Boolean | `TRUE` if added post-boil (e.g., late sugar/extract). |
| `ORIGIN` | No | Text | Country of origin. |
| `SUPPLIER` | No | Text | Maltster / manufacturer. |
| `COARSE_FINE_DIFF`| No | Float (%) | Coarse vs fine grind extract difference. |
| `MOISTURE` | No | Float (%) | Grain moisture percentage. |
| `DIASTATIC_POWER` | No | Float (°L)| Diastatic power in degrees Lintner. |
| `PROTEIN` | No | Float (%) | Total protein content. |
| `MAX_IN_BATCH` | No | Float (%) | Maximum recommended batch percentage. |
| `RECOMMEND_MASH`| No | Boolean | `TRUE` if enzymatic conversion is required. |
| `IBU_GAL_PER_LB` | No | Float | Bittering potential if pre-hopped extract. |

#### 4. `<YEAST>` Record
| Tag | Required | Type | Description |
| :--- | :--- | :--- | :--- |
| `NAME` | **Yes** | Text | Yeast / strain name. |
| `VERSION` | **Yes** | Integer | Must be `1`. |
| `TYPE` | **Yes** | Enum | `Ale`, `Lager`, `Wheat`, `Wine`, `Champagne`. |
| `FORM` | **Yes** | Enum | `Liquid`, `Dry`, `Slant`, `Culture`. |
| `AMOUNT` | **Yes** | Float | Amount in liters (liquid) or kilograms (dry). |
| `AMOUNT_IS_WEIGHT`| No | Boolean | `TRUE` if `AMOUNT` is kg, `FALSE` if liters. |
| `LABORATORY` | No | Text | Producer (e.g., `White Labs`, `Wyeast`). |
| `PRODUCT_ID` | No | Text | Catalog code (e.g., `WLP001`, `WY1056`). |
| `MIN_TEMPERATURE`| No | Float (°C) | Recommended lower fermentation temperature. |
| `MAX_TEMPERATURE`| No | Float (°C) | Recommended upper fermentation temperature. |
| `FLOCCULATION` | No | Enum | `Low`, `Medium`, `High`, `Very High`. |
| `ATTENUATION` | No | Float (%) | Apparent attenuation percentage. |

#### 5. `<MISC>` Record
| Tag | Required | Type | Description |
| :--- | :--- | :--- | :--- |
| `NAME` | **Yes** | Text | Adjunct / fining / spice name. |
| `VERSION` | **Yes** | Integer | Must be `1`. |
| `TYPE` | **Yes** | Enum | `Spice`, `Fining`, `Water Agent`, `Herb`, `Flavor`, `Other`. |
| `USE` | **Yes** | Enum | `Boil`, `Mash`, `Primary`, `Secondary`, `Bottling`. |
| `TIME` | **Yes** | Float (min)| Usage contact duration. |
| `AMOUNT` | **Yes** | Float | Amount (kg if weight, L if volume). |
| `AMOUNT_IS_WEIGHT`| No | Boolean | Declares whether `AMOUNT` represents kg or L. |

#### 6. `<MASH>` & `<MASH_STEP>` Records
* `<MASH>` container fields: `NAME`, `VERSION`, `GRAIN_TEMP`, `MASH_STEPS` (record set).
* `<MASH_STEP>` fields:
  * `NAME`: Name of the step (e.g., `Saccharification Rest`).
  * `TYPE`: `Infusion`, `Temperature`, or `Decoction`.
  * `STEP_TEMP`: Target temperature in °C.
  * `STEP_TIME`: Hold time in minutes.
  * `INFUSE_AMOUNT`: Volume of water added in liters (for Infusion steps).
  * `RAMP_TIME`: Minutes required to reach target temperature.

---

## 3. BeerJSON Specification Reference

### 3.1 Document Schema & Root Layout
BeerJSON is defined by formal JSON Schema definitions. All documents must have a single root object `beerjson` containing a `version` property and optional top-level arrays:

```json
{
  "beerjson": {
    "version": 2.06,
    "recipes": [],
    "fermentables": [],
    "hop_varieties": [],
    "cultures": [],
    "miscellaneous_ingredients": [],
    "water_profiles": [],
    "mashes": [],
    "boil": [],
    "fermentations": [],
    "equipments": [],
    "packaging": [],
    "styles": []
  }
}
```

### 3.2 Measurable Unit Architecture
BeerJSON enforces explicit scalar typing using structured unit objects. Rather than raw numbers, measurable physical attributes are encoded as:

```json
{
  "value": 5.0,
  "unit": "kg"
}
```

#### Supported Unit Types & Dimensions:
| Measurement Type | Recognized Unit Strings |
| :--- | :--- |
| **MassType** | `mg`, `g`, `kg`, `lb`, `oz` |
| **VolumeType** | `ml`, `l`, `tsp`, `tbsp`, `floz`, `cup`, `pt`, `qt`, `gal`, `bbl`, `ifloz`, `ipt`, `iqt`, `igal`, `ibbl` |
| **TemperatureType**| `C`, `F` |
| **TimeType** | `sec`, `min`, `hr`, `day`, `week` |
| **GravityType** | `sg`, `plato`, `brix` |
| **ColorType** | `SRM`, `EBC`, `Lovi` |
| **PressureType** | `kPa`, `psi`, `bar` |
| **PercentType** | `%` |
| **CarbonationType**| `vols`, `g/l` |
| **AcidityType** | `pH` |

---

### 3.3 Core BeerJSON Sub-Schemas

#### 1. `RecipeType`
```json
{
  "name": "West Coast IPA",
  "type": "all grain",
  "author": "Master Brewer",
  "batch_size": { "value": 20.0, "unit": "l" },
  "efficiency": {
    "brewhouse": { "value": 75.0, "unit": "%" },
    "conversion": { "value": 95.0, "unit": "%" }
  },
  "boil": {
    "pre_boil_size": { "value": 25.5, "unit": "l" },
    "boil_time": { "value": 60, "unit": "min" }
  },
  "ingredients": {
    "fermentable_additions": [],
    "hop_additions": [],
    "culture_additions": [],
    "miscellaneous_additions": [],
    "water_additions": []
  },
  "mash": {
    "name": "Single Infusion",
    "mash_steps": []
  },
  "fermentation": {
    "name": "Standard Ale Fermentation",
    "fermentation_steps": []
  },
  "packaging": {
    "packaging_vessels": []
  }
}
```

#### 2. `FermentableAdditionType`
Inside `ingredients.fermentable_additions[]`:
```json
{
  "name": "Pilsner Malt",
  "type": "grain",
  "amount": { "value": 4.5, "unit": "kg" },
  "yield": {
    "fine_grind": { "value": 81.5, "unit": "%" },
    "potential": { "value": 1.037, "unit": "sg" }
  },
  "color": { "value": 1.8, "unit": "SRM" },
  "origin": "Germany",
  "producer": "Weyermann"
}
```

#### 3. `HopAdditionType` & Timing Object
BeerJSON separates hop identity from the addition schedule using a structured `timing` object:

```json
{
  "name": "Simcoe",
  "form": "pellet",
  "amount": { "value": 50, "unit": "g" },
  "alpha_acid": { "value": 13.0, "unit": "%" },
  "timing": {
    "use": "add_to_boil",
    "time": { "value": 15, "unit": "min" },
    "duration": { "value": 15, "unit": "min" },
    "step_temperature": { "value": 100, "unit": "C" }
  }
}
```

##### Timing `use` Enums in BeerJSON:
* `add_to_mash`
* `add_to_first_wort`
* `add_to_boil`
* `add_to_whirlpool`
* `add_to_fermentation` (Dry hopping)
* `add_to_package`

#### 4. `CultureAdditionType`
Replaces `<YEAST>` and supports multi-strain pitching and bacteria:
```json
{
  "name": "SafAle American Ale",
  "type": "ale",
  "form": "dry",
  "producer": "Fermentis",
  "product_id": "US-05",
  "attenuation": { "value": 78.0, "unit": "%" },
  "amount": { "value": 11.5, "unit": "g" }
}
```

#### 5. `FermentationProcedureType` & `FermentationStepType`
BeerJSON models fermentation as dynamic, ordered process steps rather than static days:
```json
{
  "name": "Two-Stage Ale",
  "fermentation_steps": [
    {
      "name": "Primary Rest",
      "step_temperature": { "value": 19.5, "unit": "C" },
      "step_time": { "value": 7, "unit": "day" },
      "free_rise": false
    },
    {
      "name": "Diacetyl Rest",
      "step_temperature": { "value": 22.0, "unit": "C" },
      "step_time": { "value": 3, "unit": "day" }
    },
    {
      "name": "Cold Crash",
      "step_temperature": { "value": 2.0, "unit": "C" },
      "step_time": { "value": 4, "unit": "day" }
    }
  ]
}
```

---

## 4. Semantic Mapping & Crosswalk Engine

### 4.1 Recipe Header Field Mapping

| BeerXML 1.0 Element | BeerJSON Path | Conversion Note |
| :--- | :--- | :--- |
| `<NAME>` | `recipe.name` | Direct string copy |
| `<TYPE>` | `recipe.type` | Normalize case: `All Grain` -> `all grain`, `Extract` -> `extract` |
| `<BREWER>` | `recipe.author` | Direct string copy |
| `<BATCH_SIZE>` | `recipe.batch_size` | Float `val` -> `{"value": val, "unit": "l"}` |
| `<BOIL_SIZE>` | `recipe.boil.pre_boil_size`| Float `val` -> `{"value": val, "unit": "l"}` |
| `<BOIL_TIME>` | `recipe.boil.boil_time` | Float `val` -> `{"value": val, "unit": "min"}` |
| `<EFFICIENCY>` | `recipe.efficiency.brewhouse`| Float `val` -> `{"value": val, "unit": "%"}` |
| `<NOTES>` | `recipe.notes` | Direct string copy |

---

### 4.2 Hop Usage & Timing Translation Matrix

| BeerXML `<USE>` | BeerXML `<TIME>` | BeerJSON `timing.use` | BeerJSON Timing Structure |
| :--- | :--- | :--- | :--- |
| `Boil` | Boil minutes left ($T$) | `add_to_boil` | `time`: `{"value": T, "unit": "min"}` |
| `First Wort` | Total boil time | `add_to_first_wort` | `time`: `{"value": T, "unit": "min"}` |
| `Mash` | Mash rest time | `add_to_mash` | `time`: `{"value": T, "unit": "min"}` |
| `Aroma` | Whirlpool duration | `add_to_whirlpool` | `duration`: `{"value": T, "unit": "min"}` |
| `Dry Hop` | Contact days ($D$) | `add_to_fermentation` | `time`: `{"value": D, "unit": "day"}` |

---

### 4.3 Fermentation Schedule Translation

#### BeerXML -> BeerJSON
```
1. If PRIMARY_AGE > 0:
   Append Step 1:
     name: "Primary Fermentation"
     step_time: { value: PRIMARY_AGE, unit: "day" }
     step_temperature: { value: PRIMARY_TEMP, unit: "C" }

2. If SECONDARY_AGE > 0:
   Append Step 2:
     name: "Secondary Fermentation"
     step_time: { value: SECONDARY_AGE, unit: "day" }
     step_temperature: { value: SECONDARY_TEMP, unit: "C" }

3. If TERTIARY_AGE > 0:
   Append Step 3:
     name: "Conditioning"
     step_time: { value: TERTIARY_AGE, unit: "day" }
     step_temperature: { value: TERTIARY_TEMP, unit: "C" }
```

#### BeerJSON -> BeerXML
```
1. Scan fermentation_steps array:
   - Step 0 -> PRIMARY_AGE = step_time (days), PRIMARY_TEMP = step_temperature (C)
   - Step 1 -> SECONDARY_AGE = step_time (days), SECONDARY_TEMP = step_temperature (C)
   - Step 2 -> TERTIARY_AGE = step_time (days), TERTIARY_TEMP = step_temperature (C)
2. If time unit is 'hr' or 'week', normalize to 'day'.
```

---

## 5. Unit Conversion Mathematics & Precision Rules

When converting BeerJSON recipes (which may contain imperial or non-standard metric units) into BeerXML 1.0 (which strictly requires metric SI floats), use the following exact constants:

### 5.1 Mass Conversions (to Kilograms)
$$kg = \text{value} \times K_{\text{mass}}$$

| Source Unit | Multiplier ($K_{\text{mass}}$) |
| :--- | :--- |
| `kg` | $1.0$ |
| `g` | $0.001$ |
| `mg` | $0.000001$ |
| `lb` | $0.45359237$ |
| `oz` | $0.028349523125$ |

### 5.2 Volume Conversions (to Liters)
$$L = \text{value} \times K_{\text{vol}}$$

| Source Unit | Multiplier ($K_{\text{vol}}$) |
| :--- | :--- |
| `l` | $1.0$ |
| `ml` | $0.001$ |
| `gal` (US liquid) | $3.785411784$ |
| `floz` (US fluid) | $0.0295735295625$ |
| `qt` (US quart) | $0.946352946$ |
| `pt` (US pint) | $0.473176473$ |
| `bbl` (US beer barrel) | $117.347$ |
| `igal` (Imperial gal) | $4.54609$ |

### 5.3 Temperature Conversions (to Celsius)
$$C = \begin{cases} \text{value}, & \text{if unit is } C \\ (\text{value} - 32) \times \frac{5}{9}, & \text{if unit is } F \end{cases}$$

### 5.4 Specific Gravity, Plato, and Brix
If BeerJSON specifies Plato or Brix, calculate Specific Gravity ($SG$) using Lincoln's equation:
$$SG \approx 1 + \left(\frac{^{\circ}P}{258.6 - \left(\frac{^{\circ}P}{258.2}\right) \times 227.1}\right)$$
Or the polynomial standard:
$$SG \approx \frac{^{\circ}P}{258.6 - (^{\circ}P \times 0.88)} + 1.000$$

---

## 6. Python Conversion Reference Engine

Below is a reference Python implementation for bidirectional BeerXML <-> BeerJSON conversion:

```python
import xml.etree.ElementTree as ET
import json
from typing import Dict, Any, List

def beerxml_to_beerjson(xml_root: ET.Element) -> Dict[str, Any]:
    # Converts a BeerXML ElementTree root into BeerJSON format
    recipes_node = xml_root.findall(".//RECIPE")
    json_recipes = []

    for r in recipes_node:
        def get_text(node, tag, default=""):
            el = node.find(tag)
            return el.text.strip() if el is not None and el.text else default

        def get_float(node, tag, default=0.0):
            el = node.find(tag)
            try:
                return float(el.text.strip()) if el is not None and el.text else default
            except ValueError:
                return default

        recipe_dict = {
            "name": get_text(r, "NAME", "Untitled Recipe"),
            "type": get_text(r, "TYPE", "all grain").lower(),
            "author": get_text(r, "BREWER", "Unknown"),
            "batch_size": {"value": get_float(r, "BATCH_SIZE"), "unit": "l"},
            "boil": {
                "pre_boil_size": {"value": get_float(r, "BOIL_SIZE"), "unit": "l"},
                "boil_time": {"value": get_float(r, "BOIL_TIME", 60.0), "unit": "min"}
            },
            "efficiency": {
                "brewhouse": {"value": get_float(r, "EFFICIENCY", 75.0), "unit": "%"}
            },
            "ingredients": {
                "fermentable_additions": [],
                "hop_additions": [],
                "culture_additions": [],
                "miscellaneous_additions": []
            },
            "fermentation": {
                "name": "Standard Fermentation",
                "fermentation_steps": []
            }
        }

        # Fermentables
        for f in r.findall(".//FERMENTABLE"):
            recipe_dict["ingredients"]["fermentable_additions"].append({
                "name": get_text(f, "NAME"),
                "type": get_text(f, "TYPE", "grain").lower(),
                "amount": {"value": get_float(f, "AMOUNT"), "unit": "kg"},
                "color": {"value": get_float(f, "COLOR"), "unit": "SRM"},
                "yield": {
                    "fine_grind": {"value": get_float(f, "YIELD"), "unit": "%"}
                }
            })

        # Hops
        for h in r.findall(".//HOP"):
            use_raw = get_text(h, "USE", "Boil")
            use_map = {
                "Boil": "add_to_boil",
                "Dry Hop": "add_to_fermentation",
                "Mash": "add_to_mash",
                "First Wort": "add_to_first_wort",
                "Aroma": "add_to_whirlpool"
            }
            time_val = get_float(h, "TIME")
            time_unit = "day" if use_raw == "Dry Hop" else "min"

            recipe_dict["ingredients"]["hop_additions"].append({
                "name": get_text(h, "NAME"),
                "form": get_text(h, "FORM", "Pellet").lower(),
                "alpha_acid": {"value": get_float(h, "ALPHA"), "unit": "%"},
                "amount": {"value": get_float(h, "AMOUNT") * 1000.0, "unit": "g"},
                "timing": {
                    "use": use_map.get(use_raw, "add_to_boil"),
                    "time": {"value": time_val, "unit": time_unit}
                }
            })

        # Yeasts / Cultures
        for y in r.findall(".//YEAST"):
            recipe_dict["ingredients"]["culture_additions"].append({
                "name": get_text(y, "NAME"),
                "type": get_text(y, "TYPE", "ale").lower(),
                "form": get_text(y, "FORM", "dry").lower(),
                "producer": get_text(y, "LABORATORY"),
                "product_id": get_text(y, "PRODUCT_ID"),
                "attenuation": {"value": get_float(y, "ATTENUATION"), "unit": "%"}
            })

        # Fermentation steps
        pri_age = get_float(r, "PRIMARY_AGE")
        if pri_age > 0:
            recipe_dict["fermentation"]["fermentation_steps"].append({
                "name": "Primary",
                "step_time": {"value": pri_age, "unit": "day"},
                "step_temperature": {"value": get_float(r, "PRIMARY_TEMP", 20.0), "unit": "C"}
            })

        sec_age = get_float(r, "SECONDARY_AGE")
        if sec_age > 0:
            recipe_dict["fermentation"]["fermentation_steps"].append({
                "name": "Secondary",
                "step_time": {"value": sec_age, "unit": "day"},
                "step_temperature": {"value": get_float(r, "SECONDARY_TEMP", 20.0), "unit": "C"}
            })

        json_recipes.append(recipe_dict)

    return {"beerjson": {"version": 2.06, "recipes": json_recipes}}
```

---

## 7. Edge Cases, Nuances & Anti-Patterns

### 7.1 Common Pitfalls
1. **Case Sensitivity Mismatch:**
   * BeerXML tags are uppercase: `<AMOUNT>` is valid, `<Amount>` or `<amount>` violates the spec.
   * BeerJSON properties are strict snake_case: `step_temperature` is valid, `stepTemperature` or `STEP_TEMPERATURE` fails validation.
2. **Boolean Capitalization:**
   * BeerXML expects capitalized string tokens: `TRUE` or `FALSE`.
   * BeerJSON expects native JSON booleans: `true` or `false`.
3. **Hop Alpha Acid Scale:**
   * Never convert percentages to ratios (e.g. do NOT write `0.055` for 5.5% alpha). Both BeerXML and BeerJSON record alpha percentages on a `0 - 100` scale.
4. **Hop Mass Units in Conversion:**
   * BeerXML requires hop mass in **kilograms** (e.g., 28.35 grams = `0.02835`). Emitting `28.35` in BeerXML creates an absurdly bitter beer with 28.35 kg of hops. Always verify SI scale.
5. **Missing Wrapper Tags in BeerXML:**
   * Standalone ingredient exports must be wrapped in their plural collection tag (e.g., `<HOPS><HOP>...</HOP></HOPS>`). An uncontained `<HOP>` record is invalid BeerXML.

---

## 8. Agent Step-by-Step Execution Playbook

When given a recipe document to process:

1. **Format Identification:**
   * Inspect the document structure. If it begins with `<?xml` or contains tags like `<RECIPES>` or `<HOP>`, route to the **BeerXML Parser**.
   * If it is a JSON document containing a top-level `"beerjson"` key, route to the **BeerJSON Parser**.

2. **Validation:**
   * For BeerXML: Ensure all tags are uppercase, numeric values are metric SI, and required fields (`NAME`, `VERSION`, `TYPE`, `BATCH_SIZE`) exist.
   * For BeerJSON: Validate against the BeerJSON 2.x JSON schema; check that all physical quantities contain both `value` and `unit`.

3. **Transformation / Translation:**
   * Apply Section 4 crosswalk logic.
   * Normalize units into destination requirements (Section 5).
   * Transform fermentation stages between flat days and ordered step objects.

4. **Sanity Check Calculations:**
   * Compute expected original gravity ($OG$) from fermentable weights and batch size.
   * Estimate bitterness ($IBU$) using Tinseth or Rager formulas to ensure hops were not scaled incorrectly by orders of magnitude.
