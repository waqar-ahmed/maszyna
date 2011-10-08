//---------------------------------------------------------------------------
/*
    MaSzyna EU07 locomotive simulator
    Copyright (C) 2001-2004  Marcin Wozniak, Maciej Czapkiewicz and others

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/


#include "system.hpp"
#include "classes.hpp"
#pragma hdrstop

#include "Driver.h"
#include <mtable.hpp>
#include "DynObj.h"
#include <math.h>
#include "Globals.h"
/*

Modu� obs�uguj�cy sterowanie pojazdami (sk�adami poci�g�w, samochodami).
Ma dzia�a� zar�wno jako AI oraz przy prowadzeniu przez cz�owieka. W tym
drugim przypadku jedynie informuje za pomoc� napis�w o tym, co by zrobi�
w tym pierwszym. Obejmuje zar�wno maszynist� jak i kierownika poci�gu
(dawanie sygna�u do odjazdu).

Przeniesiona tutaj zosta�a zawarto�� ai_driver.pas przerobione na C++.
R�wnie� niekt�re funkcje dotycz�ce sk�ad�w z DynObj.cpp.

*/


//zrobione:
//0. pobieranie komend z dwoma parametrami
//1. przyspieszanie do zadanej predkosci, ew. hamowanie jesli przekroczona
//2. hamowanie na zadanym odcinku do zadanej predkosci (ze stabilizacja przyspieszenia)
//3. wychodzenie z sytuacji awaryjnych: bezpiecznik nadmiarowy, poslizg
//4. przygotowanie pojazdu do drogi, zmiana kierunku ruchu
//5. dwa sposoby jazdy - manewrowy i pociagowy
//6. dwa zestawy psychiki: spokojny i agresywny
//7. przejscie na zestaw spokojny jesli wystepuje duzo poslizgow lub wybic nadmiarowego.
//8. lagodne ruszanie (przedluzony czas reakcji na 2 pierwszych nastawnikach)
//9. unikanie jazdy na oporach rozruchowych
//10. logowanie fizyki
//11. kasowanie czuwaka/SHP
//12. procedury wspomagajace "patrzenie" na odlegle semafory
//13. ulepszone procedury sterowania
//14. zglaszanie problemow z dlugim staniem na sygnale S1
//15. sterowanie EN57
//16. zmiana kierunku
//17. otwieranie/zamykanie drzwi

//do zrobienia:
//1. kierownik pociagu
//2. madrzejsze unikanie grzania oporow rozruchowych i silnika
//3. unikanie szarpniec, zerwania pociagu itp
//4. obsluga innych awarii
//5. raportowanie problemow, usterek nie do rozwiazania
//6. dla Humandriver: tasma szybkosciomierza - zapis do pliku!
//7. samouczacy sie algorytm hamowania

//sta�e
const double EasyReactionTime=0.4;
const double HardReactionTime=0.2;
const double EasyAcceleration=0.5;
const double HardAcceleration=0.9;
const double PrepareTime     =2.0;
bool WriteLogFlag=false;

AnsiString StopReasonTable[]=
{//przyczyny zatrzymania ruchu AI
 "", //stopNone, //nie ma powodu - powinien jecha�
 "Sleep", //stopSleep, //nie zosta� odpalony, to nie pojedzie
 "Semaphore", //stopSem, //semafor zamkni�ty
 "Time", //stopTime, //czekanie na godzin� odjazdu
 "End of track", //stopEnd, //brak dalszej cz�ci toru
 "Change direction", //stopDir, //trzeba stan��, by zmieni� kierunek jazdy
 "Block", //stopBlock, //przeszkoda na drodze ruchu
 "A command", //stopComm, //otrzymano tak� komend� (niewiadomego pochodzenia)
 "Out of station", //stopOut, //komenda wyjazdu poza stacj� (raczej nie powinna zatrzymywa�!)
 "Radiostop", //stopRadio, //komunikat przekazany radiem (Radiostop)
 "Error", //stopError //z powodu b��du w obliczeniu drogi hamowania
};

AnsiString __fastcall Order2Str(TOrders Order)
{//zamiana kodu rozkazu na opis
 switch (Order)
 {
  Wait_for_orders:     return "Wait_for_orders";
  Prepare_engine:      return "Prepare_engine";
  Shunt:               return "Shunt";
  Change_direction:    return "Change_direction";
  Obey_train:          return "Obey_train";
  Release_engine:      return "Release_engine";
  Jump_to_first_order: return "Jump_to_first_order";
 }
 return "Undefined!";
}

AnsiString __fastcall TController::OrderCurrent()
{//pobranie aktualnego rozkazu celem wy�wietlenia
 return Order2Str(OrderList[OrderPos]);
};

bool __fastcall TController::CheckSKP()
{//sprawdzenie potrzeby za�o�enia SKP
 return bCheckSKP;
}

void __fastcall TController::ResetSKP()
{//skasowanie potrzeby za�o�enia SKP
 bCheckSKP=false;
}

int __fastcall TController::OrderDirectionChange(int newdir,Mover::TMoverParameters *Vehicle)
{//zmiana kierunku
 int testd;
 testd=newdir;
 //with Vehicle do
 if (Vehicle->Vel<0.1)
 {
  switch (newdir)
  {
   case -1: if (!Vehicle->DirectionBackward()) testd=0; break;
   case  1: if (!Vehicle->DirectionForward()) testd=0; break;
  }
  if (testd==0)
   VelforDriver=-1;
 }
 else
  VelforDriver=0;
 if ((Vehicle->ActiveDir==0)&&(VelforDriver<Vehicle->Vel))
  IncBrake();
 if (Vehicle->ActiveDir==testd)
  VelforDriver=-1;
 if ((Vehicle->ActiveDir>0)&&(Vehicle->TrainType==dt_EZT))
  Vehicle->DirectionForward(); //Ra: z przekazaniem do silnikowego
 return (int)VelforDriver;
}

void __fastcall TController::WaitingSet(double Seconds)
{//ustawienie odczekania
 WaitingTime=-Seconds;
}

void __fastcall TController::SetVelocity(double NewVel,double NewVelNext,TStopReason r)
{//ustawienie nowej pr�dko�ci
 WaitingTime=-WaitingExpireTime; //no albo przypisujemy -WaitingExpireTime, albo por�wnujemy z WaitingExpireTime
 MaxVelFlag=False;
 MinVelFlag=False;
 VelActual=NewVel;   //pr�dko�� do kt�rej d��y
 VelNext=NewVelNext; //pr�dko�� przy nast�pnym obiekcie
 if ((NewVel>NewVelNext) //je�li oczekiwana wi�ksza ni� nast�pna
  || (NewVel<Controlling->Vel)) //albo aktualna jest mniejsza ni� aktualna
  ProximityDist=-800.0; //droga hamowania do zmiany pr�dko�ci
 else
  ProximityDist=-300.0;
 if (NewVel==0.0) //je�li ma stan��
 {if (r!=stopNone) //a jest pow�d podany
   eStopReason=r; //to zapami�ta� nowy pow�d
 }
 else
  eStopReason=stopNone; //podana pr�dko��, to nie ma powod�w do stania
}

bool __fastcall TController::SetProximityVelocity(double NewDist,double NewVelNext)
{//informacja o pr�dko�ci w pewnej odleg�o�ci
 if (NewVelNext==0.0)
  WaitingTime=0.0; //nie trzeba ju� czeka�
 //if ((NewVelNext>=0)&&((VelNext>=0)&&(NewVelNext<VelNext))||(NewVelNext<VelActual))||(VelNext<0))
 {MaxVelFlag=False;
  MinVelFlag=False;
  VelNext=NewVelNext;
  ProximityDist=NewDist;
  return true;
 }
 //else return false
}

void __fastcall TController::SetDriverPsyche()
{
 double maxdist=0.5; //skalowanie dystansu od innego pojazdu, zmienic to!!!
 if ((Psyche==Aggressive)&&(OrderList[OrderPos]==Obey_train))
 {
  ReactionTime=HardReactionTime; //w zaleznosci od charakteru maszynisty
  AccPreferred=HardAcceleration; //agresywny
  if (Controlling!=NULL)
   if (Controlling->CategoryFlag==2)
    WaitingExpireTime=11;
   else
    WaitingExpireTime=61;
 }
 else
 {
  ReactionTime=EasyReactionTime; //spokojny
  AccPreferred=EasyAcceleration;
  WaitingExpireTime=65;
 }
 if (Controlling!=NULL)
 {//with Controlling do
  if (Controlling->MainCtrlPos<3)
   ReactionTime=Controlling->InitialCtrlDelay+ReactionTime;
  if (Controlling->BrakeCtrlPos>1)
   ReactionTime=0.5*ReactionTime;
  if ((Controlling->V>0.1)&&(Controlling->Couplers[0].Connected)) //dopisac to samo dla V<-0.1 i zaleznie od Psyche
   if (Controlling->Couplers[0].CouplingFlag==0)
   {//Ra: funkcje s� odpowiednie?
    AccPreferred=((Mover::TMoverParameters*)(Controlling->Couplers[0].Connected))->V; //tymczasowa warto��
    AccPreferred=(AccPreferred*AccPreferred-Controlling->V*Controlling->V)/(25.92*(Controlling->Couplers[0].Dist-maxdist*fabs(Controlling->V)));
    AccPreferred=Min0R(AccPreferred,EasyAcceleration);
   }
 }
};

bool __fastcall TController::PrepareEngine()
{//odpalanie silnika
 bool OK;
 bool voltfront,voltrear;
 voltfront=false;
 voltrear=false;
 LastReactionTime=0.0;
 ReactionTime=PrepareTime;
 //with Controlling do
 if (((Controlling->EnginePowerSource.SourceType==CurrentCollector)||(Controlling->TrainType==dt_EZT)))
 {
  if (Controlling->GetTrainsetVoltage())
  {
   voltfront=true;
   voltrear=true;
  }
 }
//   begin
//     if Couplers[0].Connected<>nil)
//     begin
//       if Couplers[0].Connected^.PantFrontVolt or Couplers[0].Connected^.PantRearVolt)
//         voltfront:=true
//       else
//         voltfront:=false;
//     end
//     else
//        voltfront:=false;
//     if Couplers[1].Connected<>nil)
//     begin
//      if Couplers[1].Connected^.PantFrontVolt or Couplers[1].Connected^.PantRearVolt)
//        voltrear:=true
//      else
//        voltrear:=false;
//     end
//     else
//        voltrear:=false;
//   end
 else
  //if EnginePowerSource.SourceType<>CurrentCollector)
  if (Controlling->TrainType!=dt_EZT)
   voltfront=true;
 if (Controlling->EnginePowerSource.SourceType==CurrentCollector)
 {
  Controlling->PantFront(true);
  Controlling->PantRear(true);
 }
 if (Controlling->TrainType==dt_EZT)
 {
  Controlling->PantFront(true);
  Controlling->PantRear(true);
 }
 //Ra: rozdziawianie drzwi po odpaleniu nie jest potrzebne
 //if (Controlling->DoorOpenCtrl==1)
 // if (!Controlling->DoorRightOpened)
 //  Controlling->DoorRight(true);  //McZapkie: taka prowizorka bo powinien wiedziec gdzie peron
//voltfront:=true;
 if (Controlling->PantFrontVolt||Controlling->PantRearVolt||voltfront||voltrear)
 {
  if (!Controlling->Mains)
  {
   //if TrainType=dt_SN61)
   //   begin
   //      OK:=(OrderDirectionChange(ChangeDir,Controlling)=-1);
   //      OK:=IncMainCtrl(1);
   //   end;
   OK=Controlling->MainSwitch(true);
  }
  else
  {
   OK=(OrderDirectionChange(ChangeDir,Controlling)==-1);
   Controlling->CompressorSwitch(true);
   Controlling->ConverterSwitch(true);
   Controlling->CompressorSwitch(true);
  }
  if (Controlling->V==0)
  {//ustalenie kierunku, gdy stoi
   if (Controlling->Couplers[1].CouplingFlag==ctrain_virtual) //je�li z ty�u nie ma nic
    if (Controlling->PantFrontVolt||Controlling->PantRearVolt||voltfront||voltrear)
     ChangeDir=-1;
   if (Controlling->Couplers[0].CouplingFlag==ctrain_virtual) //je�li z przodu nie ma nic
    if (Controlling->PantFrontVolt||Controlling->PantRearVolt||voltfront||voltrear)
     ChangeDir=1;
  }
  else //ustalenie kierunku, gdy jedzie
   if (Controlling->V<0) //jedzie do ty�u
    if (Controlling->PantFrontVolt||Controlling->PantRearVolt||voltfront||voltrear)
     ChangeDir=-1;
    else //jak nie do ty�u, to do przodu
     if (Controlling->PantFrontVolt||Controlling->PantRearVolt||voltfront||voltrear)
      ChangeDir=1;
 }
 else
  OK=false;
 OK=OK&&(Controlling->ActiveDir!=0)&&(Controlling->CompressorAllow);
 if (OK)
 {
  if (eStopReason==stopSleep) //je�li dotychczas spa�
   eStopReason==stopNone; //teraz nie ma powodu do stania
  EngineActive=true;
  return true;
 }
 else
 {
  EngineActive=false;
  return false;
 }
};

bool __fastcall TController::ReleaseEngine()
{//wy��czanie silnika
 bool OK=false;
 LastReactionTime=0.0;
 ReactionTime=PrepareTime;
 if (Controlling->DoorOpenCtrl==1)
 {//zamykanie drzwi
  if (Controlling->DoorLeftOpened) Controlling->DoorLeft(false);
  if (Controlling->DoorRightOpened) Controlling->DoorRight(false);
 }
 if (Controlling->ActiveDir==0)
  if (Controlling->Mains)
  {
   Controlling->CompressorSwitch(false);
   Controlling->ConverterSwitch(false);
   if (Controlling->EnginePowerSource.SourceType==CurrentCollector)
   {
    Controlling->PantFront(false);
    Controlling->PantRear(false);
   }
   OK=Controlling->MainSwitch(false);
  }
  else
   OK=true;
 if (!Controlling->DecBrakeLevel())
  if (!Controlling->IncLocalBrakeLevel(1))
  {
   if (Controlling->ActiveDir==1)
    if (!Controlling->DecScndCtrl(2))
     if (!Controlling->DecMainCtrl(2))
      Controlling->DirectionBackward();
   if (Controlling->ActiveDir==-1)
    if (!Controlling->DecScndCtrl(2))
     if (!Controlling->DecMainCtrl(2))
      Controlling->DirectionForward();
  }
 if (OK&&(Controlling->Vel<0.01)) EngineActive=false;
 eStopReason=stopSleep; //stoimy z powodu wy��czenia
 return OK&&(Controlling->Vel<0.01);
}

bool __fastcall TController::IncBrake()
{//zwi�kszenie hamowania
 bool OK=false;
 //ClearPendingExceptions;
 switch (Controlling->BrakeSystem)
 {
  case Individual:
   OK=Controlling->IncLocalBrakeLevel(1+floor(0.5+fabs(AccDesired)));
   break;
  case Pneumatic:
   if ((Controlling->Couplers[0].Connected==NULL)&&(Controlling->Couplers[1].Connected==NULL))
    OK=Controlling->IncLocalBrakeLevel(1+floor(0.5+fabs(AccDesired))); //hamowanie lokalnym bo luzem jedzie
   else
   {if (Controlling->BrakeCtrlPos+1==Controlling->BrakeCtrlPosNo)
    {
     if (AccDesired<-1.5) //hamowanie nagle
      OK=Controlling->IncBrakeLevel();
     else
      OK=false;
    }
    else
    {
/*
    if (AccDesired>-0.2) and ((Vel<20) or (Vel-VelNext<10)))
            begin
              if BrakeCtrlPos>0)
               OK:=IncBrakeLevel
              else;
               OK:=IncLocalBrakeLevel(1);   //finezyjne hamowanie lokalnym
             end
           else
*/
     OK=Controlling->IncBrakeLevel();
    }
   }
   break;
  case ElectroPneumatic:
   if (Controlling->BrakeCtrlPos<Controlling->BrakeCtrlPosNo)
    if (Controlling->BrakePressureTable[Controlling->BrakeCtrlPos+1+2].BrakeType==ElectroPneumatic) //+2 to indeks Pascala 
     OK=Controlling->IncBrakeLevel();
    else
     OK=false;
  }
 return OK;
}

bool __fastcall TController::DecBrake()
{//zmniejszenie si�y hamowania
 bool OK=false;
 //ClearPendingExceptions;
 switch (Controlling->BrakeSystem)
 {
  case Individual:
   OK=Controlling->DecLocalBrakeLevel(1+floor(0.5+fabs(AccDesired)));
   break;
  case Pneumatic:
   if (Controlling->BrakeCtrlPos>0)
    OK=Controlling->DecBrakeLevel();
   if (!OK)
    OK=Controlling->DecLocalBrakeLevel(2);
   Need_BrakeRelease=true;
   break;
  case ElectroPneumatic:
   if (Controlling->BrakeCtrlPos>-1)
    if (Controlling->BrakePressureTable[Controlling->BrakeCtrlPos-1+2].BrakeType==ElectroPneumatic) //+2 to indeks Pascala
     OK=Controlling->DecBrakeLevel();
    else
    {if ((Controlling->BrakeSubsystem==Knorr)||(Controlling->BrakeSubsystem==Hik)||(Controlling->BrakeSubsystem==Kk))
     {//je�li Knorr
      Controlling->SwitchEPBrake((Controlling->BrakePress>0.0)?1:0);
     }
     OK=false;
    }
    if (!OK)
     OK=Controlling->DecLocalBrakeLevel(2);
   break;
 }
 return OK;
};

bool __fastcall TController::IncSpeed()
{//zwi�kszenie pr�dko�ci
 bool OK=true;
 //ClearPendingExceptions;
 if ((Controlling->DoorOpenCtrl==1)&&(Controlling->Vel==0)&&(Controlling->DoorLeftOpened||Controlling->DoorRightOpened))  //AI zamyka drzwi przed odjazdem
 {
  Controlling->DepartureSignal=true; //za��cenie bzyczka
  Controlling->DoorLeft(false); //zamykanie drzwi
  Controlling->DoorRight(false);
  //DepartureSignal:=false;
  //Ra: trzeba by ustawi� jaki� czas oczekiwania na zamkni�cie si� drzwi
 }
 else
 {
  Controlling->DepartureSignal=false;
  switch (Controlling->EngineType)
  {
   case None:
    if (Controlling->MainCtrlPosNo>0) //McZapkie-041003: wagon sterowniczy
    {
     //TODO: sprawdzanie innego czlonu //if (!FuseFlagCheck())
     if (Controlling->BrakePress<0.05*Controlling->MaxBrakePress)
     {
      if (Controlling->ActiveDir>0) Controlling->DirectionForward(); //zeby EN57 jechaly na drugiej nastawie
      OK=Controlling->IncMainCtrl(1);
     }
    }
    break;
   case ElectricSeriesMotor :
    if (!Controlling->FuseFlag)
     if ((Controlling->Im<=Controlling->Imin)&&Ready) //{(Controlling->BrakePress<=0.01*Controlling->MaxBrakePress)}
     {
      OK=Controlling->IncMainCtrl(1);
      if ((Controlling->MainCtrlPos>2)&&(Controlling->Im==0))
        Need_TryAgain=true;
       else
        if (!OK)
         OK=Controlling->IncScndCtrl(1); //TODO: dorobic boczniki na szeregowej przy ciezkich bruttach
     }
    break;
   case Dumb:
   case DieselElectric:
    if (Ready) //{(BrakePress<=0.01*MaxBrakePress)}
    {
     OK=Controlling->IncMainCtrl(1);
     if (!OK)
      OK=Controlling->IncScndCtrl(1);
    }
   case WheelsDriven:
    if (sin(Controlling->eAngle)>0)
     Controlling->IncMainCtrl(1+floor(0.5+fabs(AccDesired)));
    else
     Controlling->DecMainCtrl(1+floor(0.5+fabs(AccDesired)));
    break;
   case DieselEngine :
    if ((Controlling->Vel>Controlling->dizel_minVelfullengage)&&(Controlling->RList[Controlling->MainCtrlPos].Mn>0))
     OK=Controlling->IncMainCtrl(1);
    if (Controlling->RList[Controlling->MainCtrlPos].Mn==0)
     OK=Controlling->IncMainCtrl(1);
    if (!Controlling->Mains)
    {
     Controlling->MainSwitch(true);
     Controlling->ConverterSwitch(true);
     Controlling->CompressorSwitch(true);
    }
    break;
  }
 }
 return OK;
}

bool __fastcall TController::DecSpeed()
{//zmniejszenie pr�dko�ci (ale nie hamowanie)
 bool OK=false; //domy�lnie false, aby wysz�o z p�tli while
 switch (Controlling->EngineType)
 {
  case None:
   if (Controlling->MainCtrlPosNo>0) //McZapkie-041003: wagon sterowniczy, np. EZT
    OK=Controlling->DecMainCtrl(1+(Controlling->MainCtrlPos>2?1:0));
   break;
  case ElectricSeriesMotor:
   OK=Controlling->DecScndCtrl(2);
   if (!OK)
    OK=Controlling->DecMainCtrl(1+(Controlling->MainCtrlPos>2?1:0));
   break;
  case Dumb:
  case DieselElectric:
   OK=Controlling->DecScndCtrl(2);
   if (!OK)
    OK=Controlling->DecMainCtrl(2+(Controlling->MainCtrlPos/2));
   break;
  //WheelsDriven :
   // begin
   //  OK:=False;
   // end;
  case DieselEngine:
   if ((Controlling->Vel>Controlling->dizel_minVelfullengage))
   {
    if (Controlling->RList[Controlling->MainCtrlPos].Mn>0)
     OK=Controlling->DecMainCtrl(1);
   }
   else
    while ((Controlling->RList[Controlling->MainCtrlPos].Mn>0)&&(Controlling->MainCtrlPos>1))
     OK=Controlling->DecMainCtrl(1);
   break;
 }
 return OK;
}

void __fastcall TController::RecognizeCommand()
{//odczytuje komende przekazana lokomotywie
 bool OK=true;
 TOrders Order=OrderList[OrderPos];
 //ClearPendingExceptions;
 TCommand *c=&Controlling->CommandIn;
 if (c->Command=="Wait_for_orders")
  PutCommand(c->Command,c->Value1,c->Value2,c->Location);
  //Order=Wait_for_orders;
 else if (c->Command=="Prepare_engine")
 {
  PutCommand(c->Command,c->Value1,c->Value2,c->Location);
  //if (c->Value1==0)
  // Order=Release_engine;
  //else if (c->Value1==1)
  // Order=Prepare_engine;
  //else OK=false;
 }
 else if (c->Command=="Change_direction")
 {//if (c->Value1>0.0) ChangeDirOrder=1;
  //else if (c->Value1<0.0) ChangeDirOrder=-1;
  //else ChangeDirOrder=0;
  //Order=Change_direction;
  PutCommand(c->Command,c->Value1,c->Value2,c->Location);
 }
 //else if (c->Command=="Obey_train")
 else if (c->Command.Pos("Obey")==1)
 {
  PutCommand(c->Command,c->Value1,c->Value2,c->Location);
  //Order=Obey_train;
  //if (c->Value1>0)
  // TrainNumber=floor(c->Value1); //i co potem ???
 }
 else if (c->Command=="Shunt")
 {
  PutCommand(c->Command,c->Value1,c->Value2,c->Location);
  //Order=Shunt;
  //if (floor(c->Value1)!=VehicleCount)
  // VehicleCount=floor(c->Value1); //i co potem ? - trzeba zaprogramowac odczepianie
 }
 else if (c->Command=="SetVelocity")
 {
  CommandLocation=c->Location;
  SetVelocity(c->Value1,c->Value2,stopComm);  //bylo: nic nie rob bo SetVelocity zewnetrznie jest wywolywane przez dynobj.cpp
  if ((Order==Wait_for_orders)&&(c->Value1!=0.0))
   JumpToFirstOrder();
  else
   if ((Order==Shunt)&&(c->Value1!=0))
   {
    Order=Obey_train;
    bCheckSKP=true;
   }
 }
 else if (c->Command=="SetProximityVelocity")
 {
  PutCommand(c->Command,c->Value1,c->Value2,c->Location);
  //if (SetProximityVelocity(c->Value1,c->Value2))
  // CommandLocation=c->Location;
  //if (Order=Shunt) Order=Obey_train;
 }
 else if (c->Command=="ShuntVelocity")
 {
  //CommandLocation:=c->Location;
  if (c->Value1!=0)
   if ((Order==Obey_train)||(Order==Wait_for_orders))
    Order=Shunt;
  if (Order==Shunt)
   SetVelocity(c->Value1,c->Value2,stopComm);
 }
 else if (c->Command=="Jump_to_order")
 {
  PutCommand(c->Command,c->Value1,c->Value2,c->Location);
  //if (c->Value1==-1.0)
  // JumpToNextOrder();
  //else
  // if ((c->Value1>=0)&&(c->Value1<=maxorders))
  //  OrderPos=floor(c->Value1);
  // Order=OrderList[OrderPos];
/*
  if (WriteLogFlag)
  {
   append(AIlogFile);
   writeln(AILogFile,ElapsedTime:5:2," - new order: ",Order2Str(Order)," @ ",OrderPos);
   close(AILogFile);
  }
*/
 }
 else if (c->Command=="Warning_signal")
 {
  if (c->Value1>0)
  {
   WarningDuration=c->Value1;
   if (c->Value2>1)
    Controlling->WarningSignal=2;
   else
    Controlling->WarningSignal=1;
  }
 }
 else if (c->Command=="OutsideStation") //wskaznik W5
 {
  if (Order==Obey_train)
   SetVelocity(c->Value1,c->Value2); //koniec stacji - predkosc szlakowa
  else  //manewry - zawracaj
  {
   ChangeDirOrder=0;
   Order=Change_direction;
  }
 }
 else if (c->Command.Pos("PassengerStopPoint:")==1) //wskaznik W4
 {
  if (Order==Obey_train)
   TrainSet->UpdateMTable(GlobalTime->hh,GlobalTime->mm,c->Command.SubString(20,c->Command.Length()-20));
 }
 else
  PutCommand(c->Command,c->Value1,c->Value2,c->Location);
  //OK=false;
 if (OK)
 {
  c->Command="";
  OrderList[OrderPos]=Order;
 }
}

void __fastcall TController::PutCommand(AnsiString NewCommand,double NewValue1,double NewValue2,const Mover::TLocation &NewLocation,TStopReason reason)
{
 //ClearPendingExceptions;
 if (NewCommand=="SetVelocity")
 {
  CommandLocation= NewLocation;
  SetVelocity(NewValue1,NewValue2,reason);
  if ((OrderList[OrderPos]==Shunt) &&  (NewValue1!=0))
  {
   OrderList[OrderPos]=Obey_train;
   bCheckSKP=true;
  }
 }
 else if (NewCommand=="SetProximityVelocity")
 {
  if (SetProximityVelocity(NewValue1,NewValue2))
   CommandLocation=NewLocation;
  //if Order=Shunt) Order=Obey_train;
 }
 else if (NewCommand=="ShuntVelocity")
 {//Ra: by�y dwa, po��czy�em, ale nadal jest bez sensu
  CommandLocation=NewLocation;
  //SetVelocity(NewValue1,NewValue2,reason);
  //if (OrderList[OrderPos]=Obey_train) and (NewValue1<>0))
  if ((OrderList[OrderPos]==Prepare_engine))//je�li w trakcie odpalania
   OrderList[OrderPos+1]=Shunt; //wstawimy do nast�pnej pozycji
  else
   OrderList[OrderPos]=Shunt; //zamieniamy w aktualnej pozycji
  if (NewValue1!=0)
   VehicleCount=-2;
  Prepare2press=false;
  //if (OrderList[OrderPos]<>Obey_train))
  SetVelocity(NewValue1,NewValue2,reason);
 }
 else if (NewCommand=="Wait_for_orders")
  OrderList[OrderPos]=Wait_for_orders;
 else if (NewCommand=="Prepare_engine")
 {
  if (NewValue1==0)
   OrderList[OrderPos]=Release_engine;
  else if (NewValue1==1)
   OrderList[OrderPos]=Prepare_engine;
 }
 else if (NewCommand=="Change_direction")
 {if (NewValue1>0.0) ChangeDirOrder=1;
  else if (NewValue1<0.0) ChangeDirOrder=-1;
  else ChangeDirOrder=0;
  OrderList[OrderPos]=Change_direction;
 }
 else if (NewCommand=="Obey_train")
 {
  OrderList[OrderPos]=Obey_train;
  if (NewValue1>0)
   TrainNumber=floor(NewValue1); //i co potem ???
 }
 else if (NewCommand.Pos("Obey:")==1)
 {//przypisanie nowego rozk�adu jazdy
  OrderList[OrderPos]=Obey_train; //�e do jazdy
  NewCommand.Delete(1,5); //zostanie nazwa pliku z rozk�adem
  TrainSet->NewName(NewCommand);
  if (NewCommand!="none")
   if (!TrainSet->LoadTTfile(Global::asCurrentSceneryPath))
    Error("Cannot load timetable file "+NewCommand+"\r\nError "+ConversionError+" in position "+TrainSet->StationCount);
   else
   {//inicjacja pierwszego przystanku i pobranie jego nazwy
    TrainSet->UpdateMTable(GlobalTime->hh,GlobalTime->mm,TrainSet->NextStationName);
    Vehicle->asNextStop=TrainSet->NextStop();
    WriteLog("/* Timetable: "+TrainSet->ShowRelation());
    TMTableLine *t;
    for (int i=0;i<=TrainSet->StationCount;++i)
    {t=TrainSet->TimeTable+i;
     WriteLog(AnsiString(t->StationName)+" "+AnsiString((int)t->Ah)+":"+AnsiString((int)t->Am)+", "+AnsiString((int)t->Dh)+":"+AnsiString((int)t->Dm));
    }
    WriteLog("*/");
   }
  if (NewValue1>0)
   TrainNumber=floor(NewValue1); //i co potem ???
 }
 else if (NewCommand=="Shunt")
 {
  if (NewValue1!=VehicleCount)
   VehicleCount=floor(NewValue1); //i co potem ? - trzeba zaprogramowac odczepianie
  OrderList[OrderPos]=Shunt;
 }
 else if (NewCommand=="Jump_to_first_order")
  JumpToFirstOrder();
 else if (NewCommand=="Jump_to_order")
 {
  if (NewValue1==-1.0)
   JumpToNextOrder();
  else
   if ((NewValue1>=0)&&(NewValue1<=maxorders))
    OrderPos=floor(NewValue1);
/*
  if (WriteLogFlag)
  {
   append(AIlogFile);
   writeln(AILogFile,ElapsedTime:5:2," - new order: ",Order2Str( OrderList[OrderPos])," @ ",OrderPos);
   close(AILogFile);
  }
*/
 }
 else if (NewCommand=="Warning_signal")
 {
  if (NewValue1>0)
  {
   WarningDuration=NewValue1;
   if (NewValue2>1)
    Controlling->WarningSignal=2;
   else
    Controlling->WarningSignal=1;
  }
 }
 else if (NewCommand=="OutsideStation") //wskaznik W5
 {
  if (OrderList[OrderPos]==Obey_train)
   SetVelocity(NewValue1,NewValue2,stopOut); //koniec stacji - predkosc szlakowa
  else //manewry - zawracaj
  {
   ChangeDirOrder=0;
   OrderList[OrderPos]=Change_direction;
  }
 }
 else if (NewCommand=="Emergency_brake") //wymuszenie zatrzymania
 {//Ra: no nadal nie jest zbyt pi�knie
  SetVelocity(0,0,reason);
  Controlling->PutCommand("Emergency_brake",1.0,1.0,Controlling->Loc);
 }
};

const TDimension SignalDim={1,1,1};

bool __fastcall TController::UpdateSituation(double dt)
{//uruchamiac przynajmniej raz na sekunde
 double AbsAccS,VelReduced;
 bool UpdateOK=false;
 //yb: zeby EP nie musial sie bawic z ciesnieniem w PG
 if (AIControllFlag)
 {
  if (Controlling->BrakeSystem==ElectroPneumatic)
   Controlling->PipePress=0.5;
  if (Controlling->SlippingWheels)
  {
   Controlling->SandDoseOn();
   Controlling->SlippingWheels=false;
  }
 }
 HelpMeFlag=false;
 //Winger 020304
 if (Controlling->Vel>0)
 {if (AIControllFlag)
   if (Controlling->DoorOpenCtrl==1)
    if (Controlling->DoorRightOpened)
     Controlling->DoorRight(false);  //Winger 090304 - jak jedzie to niech zamyka drzwi
  if (Controlling->EnginePowerSource.SourceType==CurrentCollector)
   if (AIControllFlag)
    Controlling->PantFront(true);
  if (TestFlag(Controlling->CategoryFlag,2)) //je�li samoch�d
   if (fabs(Controlling->OffsetTrackH)<Controlling->Dim.W)
    if (!Controlling->ChangeOffsetH(-0.1*Controlling->Vel*dt))
     Controlling->ChangeOffsetH(0.1*Controlling->Vel*dt);
 }
 if (Controlling->EnginePowerSource.SourceType==CurrentCollector)
  if (Controlling->Vel>30)
   if (AIControllFlag)
    Controlling->PantRear(false);
 ElapsedTime=ElapsedTime+dt;
 WaitingTime=WaitingTime+dt;
 if (WriteLogFlag)
 {
  if (LastUpdatedTime>deltalog)
  {//zapis do pliku DAT
/*
     writeln(LogFile,ElapsedTime," ",abs(11.31*WheelDiameter*nrot)," ",AccS," ",Couplers[1].Dist," ",Couplers[1].CForce," ",Ft," ",Ff," ",Fb," ",BrakePress," ",PipePress," ",Im," ",MainCtrlPos,"   ",ScndCtrlPos,"   ",BrakeCtrlPos,"   ",LocalBrakePos,"   ",ActiveDir,"   ",CommandIn.Command," ",CommandIn.Value1," ",CommandIn.Value2," ",SecuritySystem.status);
*/
   if (fabs(Controlling->V)>0.1)
    deltalog=0.2;
   else deltalog=1.0;
   LastUpdatedTime=0.0;
  }
  else
   LastUpdatedTime= LastUpdatedTime+dt;
 }
 if (AIControllFlag)
 {
  //UpdateSituation:=True;
  //tu bedzie logika sterowania
  //Ra: nie wiem czemu ReactionTime potrafi dosta� 12 sekund, to jest przegi�cie, bo prze�yna ST�J
  //yB: ot� jest to jedna trzecia czasu nape�niania na towarowym; mo�e si� przyda� przy wdra�aniu hamowania, �eby nie rusza�o kranem jak g�upie
  if ((LastReactionTime>Min0R(ReactionTime,2.0)))
  {
   MechLoc=Controlling->Loc; //uwzglednic potem polozenie kabiny
   if (ProximityDist>=0)
    ActualProximityDist=Min0R(ProximityDist,Distance(MechLoc,CommandLocation,Controlling->Dim,SignalDim));
   else
    if (ProximityDist<0)
     ActualProximityDist=ProximityDist;
   if (Controlling->CommandIn.Command!="")
    if (!Controlling->RunInternalCommand()) //rozpoznaj komende bo lokomotywa jej nie rozpoznaje
     RecognizeCommand();
   if (Controlling->SecuritySystem.Status>1)
    if (!Controlling->SecuritySystemReset())
     if (TestFlag(Controlling->SecuritySystem.Status,s_ebrake)&&(Controlling->BrakeCtrlPos==0)&&(AccDesired>0.0))
      Controlling->DecBrakeLevel();
   switch (OrderList[OrderPos])
   {//na jaka odleglosc i z jaka predkoscia ma podjechac
    case Shunt:
     MinProximityDist=1.0; MaxProximityDist=10.0; //[m]
     VelReduced=5; //[km/h]
     if (Controlling->Vel==0)
      if (Controlling->ActiveDir*Controlling->CabNo<0)
      {
       if (Controlling->Couplers[1].Connected==NULL)
        Controlling->HeadSignalsFlag=16; //swiatla manewrowe
       if (Controlling->Couplers[0].Connected==NULL)
        Controlling->EndSignalsFlag=1;
      }
      else
      {
       if (Controlling->Couplers[1].Connected==NULL)
        Controlling->HeadSignalsFlag=1; //swiatla manewrowe
       if (Controlling->Couplers[0].Connected==NULL)
        Controlling->EndSignalsFlag=16;
      }
      //20.07.03 - manewrowanie wagonami
      if (VehicleCount>=0)
      {
       if (Prepare2press)
       {
        VelActual=3;
        if (Controlling->MainCtrlPos>0)
        {
         if ((Controlling->ActiveDir>0)&&(Controlling->Couplers[0].CouplingFlag>0))
         {
          Controlling->Dettach(0);
          VehicleCount=-2;
          VelActual=0.0; 	 																bCheckSKP= true;
         }
         else
          if ((Controlling->ActiveDir<0)&&(Controlling->Couplers[1].CouplingFlag>0))
          {
           Controlling->Dettach(1);
           VehicleCount=-2;
           VelActual=0.0;
          }
          else HelpMeFlag=true; //cos nie tak z kierunkiem dociskania
        }
        if (VehicleCount>=0)
         if (!Controlling->DecLocalBrakeLevel(1))
         {
          Controlling->BrakeReleaser();
          IncSpeed(); //docisnij sklad
         }
       }
       if ((Controlling->Vel==0)&&!Prepare2press)
       {//zmie� kierunek na przeciwny
        Prepare2press=true;
        if (Controlling->ActiveDir>=0)
         while (Controlling->ActiveDir>=0)
          Controlling->DirectionBackward();
        else
         while (Controlling->ActiveDir<=0)
          Controlling->DirectionForward();
       }
      } //odczepiania
      else
       if (Prepare2press&&!DecSpeed()) //nie ma nic do odczepiania
        {//ponowna zmiana kierunku
         if (Controlling->ActiveDir>=0)
          while (Controlling->ActiveDir>=0)
           Controlling->DirectionBackward();
          else
           while (Controlling->ActiveDir<=0)
            Controlling->DirectionForward();
         Prepare2press=false;
         VelActual=10.0;
        }
     break;
    case Obey_train:
     if (Controlling->CategoryFlag==1) //jazda pociagowa
     {
      MinProximityDist=30.0; MaxProximityDist=60.0; //[m]
      if ((Controlling->ActiveDir*Controlling->CabNo<0))
      {
       if ((Controlling->Couplers[0].CouplingFlag==0))
        Controlling->HeadSignalsFlag=1+4+16; //wlacz trojkat
       if ((Controlling->Couplers[1].CouplingFlag==0))
        Controlling->EndSignalsFlag=2+32; //swiatla konca pociagu i/lub blachy
      }
      else
      {
       if ((Controlling->Couplers[0].CouplingFlag==0))
        Controlling->EndSignalsFlag=1+4+16; //wlacz trojkat
       if ((Controlling->Couplers[1].CouplingFlag==0))
        Controlling->HeadSignalsFlag=2+32; //swiatla konca pociagu i/lub blachy
      }
     }
     else //samochod
     {
      MinProximityDist=5.0; MaxProximityDist=10.0; //[m]
     }
     VelReduced=4; //[km/h]
     break;
    default:
     MinProximityDist=-0.01; MaxProximityDist=2.0; //[m]
     VelReduced=1; //[km/h]
   } //switch
   switch (OrderList[OrderPos])
   {//co robi maszynista
    case Prepare_engine:
     if (PrepareEngine())
     {//gotowy do drogi?
      SetDriverPsyche();
      //Ra: na aktualnej pozycji zapisujemy manewry (po co?)
      //Controlling->CommandIn.Value1=-1;
      //Controlling->CommandIn.Value2=-1;
      //OrderList[OrderPos]:=Shunt; //Ra: to nie mo�e tak by�, bo scenerie robi� Jump_to_first_order i przechodzi w manewrowy
      JumpToNextOrder(); //w nast�pnym jest Shunt albo Obey_train
      //if OrderList[OrderPos]<>Wait_for_Orders)
      // if BrakeSystem=Pneumatic)  //napelnianie uderzeniowe na wstepie
      //  if BrakeSubsystem=Oerlikon)
      //   if (BrakeCtrlPos=0))
      //    DecBrakeLevel;
     }
     break;
    case Release_engine:
     if (ReleaseEngine()) //zdana maszyna?
      JumpToNextOrder();
     break;
    case Jump_to_first_order:
      if (OrderPos>1)
       OrderPos=1;
      else
       ++OrderPos;
     break;
    case Shunt:
    case Obey_train:
    case Change_direction:
     if (OrderList[OrderPos]==Shunt) //spokojne manewry
     {
      VelActual=Min0R(VelActual,30);
      if ((VehicleCount>=0)&&!Prepare2press)
       VelActual=0.0; //dorobic potem doczepianie
     }
     else
      SetDriverPsyche();
     //no albo przypisujemy -WaitingExpireTime, albo por�wnujemy z WaitingExpireTime
     if ((VelActual==0)&&(WaitingTime>WaitingExpireTime)&&(Controlling->RunningTrack.Velmax!=0))
     {//je�li stoi, a up�yn�� czas oczekiwania i tor ma niezerow� pr�dko��
/*
      if (WriteLogFlag)
       {
         append(AIlogFile);
         writeln(AILogFile,ElapsedTime:5:2,": ",Name," V=0 waiting time expired! (",WaitingTime:4:1,")");
         close(AILogFile);
       }
*/
      PrepareEngine();
      WaitingTime=0.0;
      VelActual=20.0;
      WarningDuration=1.5;
      Controlling->WarningSignal=1;
     }
     else if ((VelActual==0)&&(VelNext>0)&&(Controlling->Vel<1))
      VelActual=VelNext; //omijanie SBL
     if ((HelpMeFlag)||(Controlling->DamageFlag>0))
     {
      HelpMeFlag=false;
/*
      if (WriteLogFlag)
       with Controlling do
        {
          append(AIlogFile);
          writeln(AILogFile,ElapsedTime:5:2,": ",Name," HelpMe! (",DamageFlag,")");
          close(AILogFile);
        }
*/
     }
     if (OrderList[OrderPos]==Change_direction) //sprobuj zmienic kierunek
     {
      VelActual=0.0;
      if (VelActual<0.1)
      {
       if (ChangeDirOrder==0)
        ChangeDir=-(Controlling->ActiveDir*Controlling->CabNo);
       else
        ChangeDir=ChangeDirOrder;
       ChangeDirOrder=ChangeDir;
       Controlling->CabNo=ChangeDir;
       Controlling->ActiveCab=ChangeDir;
       Controlling->PantFront(true);
       Controlling->PantRear(true);
       Controlling->CommandIn.Value1=-1;
       Controlling->CommandIn.Value2=-1;
       OrderList[OrderPos]=Shunt;
       SetVelocity(20,20);
/*
       if (WriteLogFlag)
       {
        append(AIlogFile);
        writeln(AILogFile,ElapsedTime:5:2,": ",Name," Direction changed!");
        close(AILogFile);
       }
*/
       if ((Controlling->ActiveDir*Controlling->CabNo<0))
       {
        Controlling->HeadSignalsFlag=16; //swiatla manewrowe
        Controlling->EndSignalsFlag= 1;
       }
       else
       {
        Controlling->HeadSignalsFlag=1; //swiatla manewrowe
        Controlling->EndSignalsFlag=16;
       }
      }
    //else
    // VelActual:=0.0; //na wszelki wypadek niech zahamuje
     } //Change_direction
     //ustalanie zadanej predkosci
     if ((Controlling->ActiveDir!=0))
     {
      if (VelActual<0)
       VelDesired=Controlling->Vmax; //ile fabryka dala
      else
       VelDesired=Min0R(Controlling->Vmax,VelActual);
      if (Controlling->RunningTrack.Velmax>=0)
       VelDesired=Min0R(VelDesired,Controlling->RunningTrack.Velmax); //uwaga na ograniczenia szlakowej!
      if (VelforDriver>=0)
       VelDesired=Min0R(VelDesired,VelforDriver);
      if (TrainSet->CheckTrainLatency()<10.0)
       if (TrainSet->TTVmax>0.0)
        VelDesired=Min0R(VelDesired,TrainSet->TTVmax); //jesli nie spozniony to nie szybciej niz rozkladowa
      AbsAccS=Controlling->AccS; //czy sie rozpedza czy hamuje
      if (Controlling->V<0.0) AbsAccS=-AbsAccS;
      else if (Controlling->V==0.0) AbsAccS=0.0;
      //ustalanie zadanego przyspieszenia
      if ((VelNext>=0.0)&&(ActualProximityDist>=0)&&(Controlling->Vel>=VelNext)) //gdy zbliza sie i jest za szybko do NOWEGO
      {
       if (Controlling->Vel>0.0) //jesli nie stoi
       {
        if ((Controlling->Vel<VelReduced+VelNext)&&(ActualProximityDist>MaxProximityDist*(1+0.1*Controlling->Vel))) //dojedz do semafora/przeszkody
        {
         if (AccPreferred>0.0)
          AccDesired=0.5*AccPreferred;
          //VelDesired:=Min0R(VelDesired,VelReduced+VelNext);
        }
        else if (ActualProximityDist>MinProximityDist)
        {//25.92 - skorektowane, zeby ladniej hamowal
         if ((VelNext>Controlling->Vel-35.0)) //dwustopniowe hamowanie - niski przy ma�ej r�nicy
         {if ((VelNext>0)&&(VelNext>Controlling->Vel-3.0)) //je�li powolna i niewielkie przekroczenie
            AccDesired=0.0; //to olej (zacznij luzowa�)
          else  //w przeciwnym wypadku
           if ((VelNext==0.0)&&(Controlling->Vel*Controlling->Vel<0.4*ActualProximityDist)) //je�li st�jka i niewielka pr�dko��
           {if (Controlling->Vel<30.0)  //trzymaj 30 km/h
             AccDesired=0.5;
            else
             AccDesired=0.0;
           }
           else // prostu hamuj (niski stopie�)
            AccDesired=(VelNext*VelNext-Controlling->Vel*Controlling->Vel)/(25.92*ActualProximityDist+0.1); //hamuj proporcjonalnie //mniejsze op�nienie przy ma�ej r�nicy
         }
         else  //przy du�ej r�nicy wysoki stopie� (1,25 potrzebnego opoznienia)
          AccDesired=(VelNext*VelNext-Controlling->Vel*Controlling->Vel)/(20.73*ActualProximityDist+0.1); //hamuj proporcjonalnie //najpierw hamuje mocniej, potem zluzuje
         if (AccPreferred<AccDesired)
          AccDesired=AccPreferred; //(1+abs(AccDesired))
         ReactionTime=0.5*Controlling->BrakeDelay[2+2*Controlling->BrakeDelayFlag]; //aby szybkosc hamowania zalezala od przyspieszenia i opoznienia hamulcow
        }
        else
         VelDesired=Min0R(VelDesired,VelNext); //utrzymuj predkosc bo juz blisko
       }
       else  //zatrzymany
        if ((VelNext>0.0)||(ActualProximityDist>MaxProximityDist*1.2))
         if (AccPreferred>0)
          AccDesired=0.5*AccPreferred; //dociagnij do semafora
         else
          VelDesired=0.0; //stoj
      }
      else //gdy jedzie wolniej, to jest normalnie
       AccDesired=AccPreferred; //normalna jazda
   	//koniec predkosci nastepnej
      if ((VelDesired>=0)&&(Controlling->Vel>VelDesired)) //jesli jedzie za szybko do AKTUALNEGO
       if (VelDesired==0.0) //jesli stoj, to hamuj, ale i tak juz za pozno :)
        AccDesired=-0.5;
       else
        if ((Controlling->Vel<VelDesired+5)) //o 5 km/h to olej
        {if ((AccDesired>0.0))
          AccDesired=0.0;
         //else
        }
        else
         AccDesired=-0.2; //hamuj tak �rednio
      //if AccDesired>-AccPreferred)
      // Accdesired:=-AccPreferred;
      //koniec predkosci aktualnej
      if ((AccDesired>0)&&(VelNext>=0)) //wybieg b�d� lekkie hamowanie, warunki byly zamienione
       if (VelNext<Controlling->Vel-100.0) //lepiej zaczac hamowac}
        AccDesired=-0.2;
       else
        if ((VelNext<Controlling->Vel-70))
         AccDesired=0.0; //nie spiesz sie bo bedzie hamowanie
      //koniec wybiegu i hamowania
      //wlaczanie bezpiecznika
      if (Controlling->EngineType==ElectricSeriesMotor)
       if (Controlling->FuseFlag||Need_TryAgain)
        if (!Controlling->DecScndCtrl(1))
         if (!Controlling->DecMainCtrl(2))
          if (!Controlling->DecMainCtrl(1))
           if (!Controlling->FuseOn())
            HelpMeFlag=true;
           else
           {
            ++DriverFailCount;
            if (DriverFailCount>maxdriverfails)
             Psyche=Easyman;
            if (DriverFailCount>maxdriverfails*2)
             SetDriverPsyche();
           }
      if (Controlling->BrakeSystem==Pneumatic) //napelnianie uderzeniowe
       if (Controlling->BrakeSubsystem==Oerlikon)
       {
        if (Controlling->BrakeCtrlPos==-2)
         Controlling->BrakeCtrlPos=0;
        if ((Controlling->BrakeCtrlPos<0)&&(Controlling->PipeBrakePress<0.01))//{(CntrlPipePress-(Volume/BrakeVVolume/10)<0.01)})
         Controlling->IncBrakeLevel();
        if ((Controlling->BrakeCtrlPos==0)&&(AbsAccS<0.0)&&(AccDesired>0.0))
        //if FuzzyLogicAI(CntrlPipePress-PipePress,0.01,1))
         if (Controlling->PipeBrakePress>0.01)//{((Volume/BrakeVVolume/10)<0.485)})
          Controlling->DecBrakeLevel();
         else
          if (Need_BrakeRelease)
          //{
           Need_BrakeRelease=false;
           //DecBrakeLevel();
          //}
       }
      if (AccDesired>=0.0)
       while (DecBrake());  //je�li przyspieszamy, to nie hamujemy
      //Ra: zmieni�em 0.95 na 1.0 - trzeba ustali�, sk�d sie takie warto�ci bior�
      if ((AccDesired<=0.0)||(Controlling->Vel+VelMargin>VelDesired*1.0))
       while (DecSpeed()); //je�li hamujemy, to nie przyspieszamy
      //yB: usuni�te r�ne dziwne warunki, oddzielamy cz�� zadaj�c� od wykonawczej
      //zwiekszanie predkosci
      if ((AbsAccS<AccDesired)&&(Controlling->Vel<VelDesired*0.95-VelMargin)&&(AccDesired>=0.0))
      //Ra: zmieni�em 0.85 na 0.95 - trzeba ustali�, sk�d sie takie warto�ci bior�
      //if (!MaxVelFlag)
       if (!IncSpeed())
        MaxVelFlag=true;
       else
        MaxVelFlag=false;
      //if (Vel<VelDesired*0.85) and (AccDesired>0) and (EngineType=ElectricSeriesMotor)
      // and (RList[MainCtrlPos].R>0.0) and (not DelayCtrlFlag))
      // if (Im<Imin) and Ready=True {(BrakePress<0.01*MaxBrakePress)})
      //  IncMainCtrl(1); {zwieksz nastawnik skoro mozesz - tak aby sie ustawic na bezoporowej*/

      //yB: usuni�te r�ne dziwne warunki, oddzielamy cz�� zadaj�c� od wykonawczej
      //zmniejszanie predkosci
      if ((AccDesired<-0.1)&&(AbsAccS>AccDesired+0.05))
      //if not MinVelFlag)
       if (!IncBrake())
        MinVelFlag=true;
       else
       {MinVelFlag=false;
        ReactionTime=3+0.5*(Controlling->BrakeDelay[2+2*Controlling->BrakeDelayFlag]-3);
       }
      if ((AccDesired<-0.05)&&(AbsAccS<AccDesired-0.2))
      {//jak hamuje, to nie tykaj kranu za cz�sto
       DecBrake();
       ReactionTime=(Controlling->BrakeDelay[1+2*Controlling->BrakeDelayFlag])/3.0;
      }
      //Mietek-end1


      //zapobieganie poslizgowi w czlonie silnikowym
      if (Controlling->Couplers[0].Connected!=NULL)
       if (TestFlag(Controlling->Couplers[0].CouplingFlag,ctrain_controll))
        if (((Mover::TMoverParameters*)(Controlling->Couplers[0].Connected))->SlippingWheels)
         if (!Controlling->DecScndCtrl(1))
         {
          if (!Controlling->DecMainCtrl(1))
           if (Controlling->BrakeCtrlPos==Controlling->BrakeCtrlPosNo)
            Controlling->DecBrakeLevel();
          ++DriverFailCount;
         }
      //zapobieganie poslizgowi u nas
      if (Controlling->SlippingWheels)
      {
       if (!Controlling->DecScndCtrl(1))
        if (!Controlling->DecMainCtrl(1))
         if (Controlling->BrakeCtrlPos==Controlling->BrakeCtrlPosNo)
          Controlling->DecBrakeLevel();
         else
          Controlling->AntiSlippingButton();
       ++DriverFailCount;
      }
      if (DriverFailCount>maxdriverfails)
      {
       Psyche=Easyman;
       if (DriverFailCount>maxdriverfails*2)
        SetDriverPsyche();
      }
     } //kierunek r�ny od zera
     //odhamowywanie skladu po zatrzymaniu i zabezpieczanie lokomotywy
     if ((Controlling->V==0.0)&&((VelDesired==0.0)||(AccDesired==0.0)))
      if ((Controlling->BrakeCtrlPos<1)||!Controlling->DecBrakeLevel())
       Controlling->IncLocalBrakeLevel(1);

     //??? sprawdzanie przelaczania kierunku
     //if (ChangeDir!=0)
     // if (OrderDirectionChange(ChangeDir)==-1)
     //  VelActual=VelProximity;
     break; //rzeczy robione przy jezdzie
   } //switch (OrderList[OrderPos])
   //kasowanie licznika czasu
   LastReactionTime=0.0;
   //ewentualne skanowanie toru
   if (!ScanMe)
    ScanMe=true;
   UpdateOK=true;
  }
  else
   LastReactionTime=LastReactionTime+dt;
  if (WarningDuration>0.1)
  {//tr�bienie
   WarningDuration=WarningDuration-dt;
   if (WarningDuration<0.2)
    Controlling->WarningSignal=0.0;
  }
  return UpdateOK;
 }
 else //if (AIControllFlag)
  return false; //AI nie obs�uguje
}

void __fastcall TController::JumpToNextOrder()
{
 if (OrderList[OrderPos]!=Wait_for_orders)
 {
  if (OrderPos<maxorders)
   ++OrderPos;
  else
   OrderPos=0;
 }
};

void __fastcall TController::JumpToFirstOrder()
{
 OrderPos=0;
}

void __fastcall TController::ChangeOrder(TOrders NewOrder)
{
 OrderList[OrderPos]=NewOrder;
}

TOrders __fastcall TController::GetCurrentOrder()
{
 return OrderList[OrderPos];
}

__fastcall TController::TController
(const Mover::TLocation &LocInitial,
 const Mover::TRotation &RotInitial,
 bool AI,
 TDynamicObject *NewControll,
 Mtable::TTrainParameters *NewTrainSet,
 bool InitPsyche
)
{
 EngineActive=false;
 MechLoc=LocInitial;
 MechRot=RotInitial;
 LastUpdatedTime=0.0;
 ElapsedTime=0.0;
 //inicjalizacja zmiennych
 Psyche=InitPsyche;
 AccDesired=AccPreferred;
 VelDesired=0.0;
 VelforDriver=-1;
 LastReactionTime=0.0;
 HelpMeFlag=false;
 ProximityDist=1;
 ActualProximityDist=1;
 CommandLocation.X=0;
 CommandLocation.Y=0;
 CommandLocation.Z=0;
 VelActual=0.0;
 VelNext=120.0;
 AIControllFlag=AI;
 Vehicle=NewControll;
 Controlling=Vehicle->MoverParameters; //skr�t do obiektu parametr�w
 Vehicles[0]=Vehicle->GetFirstDynamic(0); //pierwszy w kierunku jazdy (Np. Pc1)
 Vehicles[1]=Vehicle->GetFirstDynamic(1); //ostatni w kierunku jazdy (ko�c�wki)
 SetDriverPsyche(); //wymaga wska�nika na pojazd (Controlling)
/*
 switch (Controlling->CabNo)
 {
  case -1: SendCtrlBroadcast("CabActivisation",1); break;
  case  1: SendCtrlBroadcast("CabActivisation",2); break;
  default: AIControllFlag:=False; //na wszelki wypadek
 }
*/
 VehicleName=Controlling->Name;
 TrainSet=NewTrainSet;
 OrderCommand="";
 OrderValue=0;
 OrderPos=0;
 for (int b=0;b<=maxorders;b++)
  OrderList[b]=Wait_for_orders;
 MaxVelFlag=false; MinVelFlag=false;
 ChangeDir=1;
 DriverFailCount=0;
 Need_TryAgain=false;
 Need_BrakeRelease=true;
 deltalog=1.0;
/*
 if (WriteLogFlag)
 {
  assignfile(LogFile,VehicleName+".dat");
  rewrite(LogFile);
   writeln(LogFile,
' Time [s]   Velocity [m/s]  Acceleration [m/ss]   Coupler.Dist[m]  Coupler.Force[N]  TractionForce [kN]  FrictionForce [kN]   BrakeForce [kN]    BrakePress [MPa]   PipePress [MPa]   MotorCurrent [A]    MCP SCP BCP LBP DmgFlag Command CVal1 CVal2');
 }
  if (WriteLogFlag)
  {
   assignfile(AILogFile,VehicleName+".txt");
   rewrite(AILogFile);
   writeln(AILogFile,"AI driver log: started OK");
   close(AILogFile);
  }
*/
 ScanMe=False;
 VelMargin=Controlling->Vmax*0.005;
 WarningDuration=-1;
 WaitingExpireTime=31.0;
 WaitingTime=0.0;
 MinProximityDist=30.0;
 MaxProximityDist=50.0;
 VehicleCount=-2;
 Prepare2press=false;
 eStopReason=stopSleep; //na pocz�tku �pi
}

void __fastcall TController::CloseLog()
{
/*
 if (WriteLogFlag)
 {
  CloseFile(LogFile);
  //if WriteLogFlag)
  // CloseFile(AILogFile);
  append(AIlogFile);
  writeln(AILogFile,ElapsedTime5:2,": QUIT");
  close(AILogFile);
 }
*/
}

AnsiString __fastcall TController::StopReasonText()
{//informacja tekstowa o przyczynie zatrzymania
 return StopReasonTable[eStopReason];
};
