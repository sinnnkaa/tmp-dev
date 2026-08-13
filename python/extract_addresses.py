import osmium
import json
import sys

address_db = {}

class AddressHandler(osmium.SimpleHandler):
    def process_tags(self, tags, lat, lon):
        if 'addr:street' in tags and 'addr:housenumber' in tags:
            street = tags['addr:street'].lower().replace("улица", "").replace("проспект", "").strip()
            number = tags['addr:housenumber'].lower().strip()

            key = f"{street} {number}"
            
            if key not in address_db:
                address_db[key] = [round(lat, 6), round(lon, 6)]

    def node(self, n):
        self.process_tags(n.tags, n.location.lat, n.location.lon)

    def area(self, a):
        try:
            for outer in a.outer_rings():
                for node in outer:
                    self.process_tags(a.tags, node.lat, node.lon)
                    return 
        except osmium.InvalidLocationError:
            pass

if __name__ == '__main__':
    pbf_file = "/root/diplom-cpp/blind_nav/map/spb.pbf"
    out_file = "/root/diplom-cpp/blind_nav/map/addresses.json"
   
    handler = AddressHandler()

    handler.apply_file(pbf_file, locations=True)
    
    print(f"Найдено адресов: {len(address_db)}")
    
    with open(out_file, 'w', encoding='utf-8') as f:
        json.dump(address_db, f, ensure_ascii=False, separators=(',', ':'))
